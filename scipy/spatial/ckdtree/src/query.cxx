/*
 * This would break SciPy with NumPy 1.6 so just accept the compiler
 * warning for now.
 * #define NPY_NO_DEPRECATED_API NPY_1_9_API_VERSION
 *
 */

#include <Python.h>
#include "numpy/arrayobject.h"

#include <cmath>
#include <cstdlib>
#include <cstring>

#include <vector>
#include <string>
#include <sstream>
#include <new>
#include <typeinfo>
#include <stdexcept>
#include <ios>

#define CKDTREE_METHODS_IMPL
#include "ckdtree_decl.h"
#include "ordered_pair.h"
#include "ckdtree_methods.h"
#include "rectangle.h"
#include "cpp_exc.h"

/*
 * Priority queue
 * ==============
 */

union heapcontents {
    npy_intp intdata;
    void     *ptrdata;
};

struct heapitem {
    npy_float64 priority;
    heapcontents contents;
};

struct heap {

    std::vector<heapitem> _heap;
    npy_intp n;
    npy_intp space;

    heap (npy_intp initial_size) : _heap(initial_size) {
        space = initial_size;
        n = 0;
    }

    inline void push(heapitem &item) {
        npy_intp i;
        heapitem t;
        n++;

        if (n > space) _heap.resize(2*space+1);
        space = _heap.size();

        i = n-1;
        _heap[i] = item;
        while ((i > 0) && (_heap[i].priority < _heap[(i-1)/2].priority)) {
            t = _heap[(i-1)/2];
            _heap[(i-1)/2] = _heap[i];
            _heap[i] = t;
            i = (i-1)/2;
        }
    }

    inline heapitem peek() {return _heap[0];}

    inline void remove() {
        heapitem t;
        npy_intp i, j, k, l, nn;
        _heap[0] = _heap[n-1];
        n--;
        /*
         * No point in freeing up space as the heap empties.
         * The whole heap gets deallocated at the end of any
         * query below. Just keep the space to avoid unneccesary
         * reallocs.
         */
        nn = n;
        i=0;
        j=1;
        k=2;
        while (((j<nn) && (_heap[i].priority > _heap[j].priority)) ||
               ((k<nn) && (_heap[i].priority > _heap[k].priority))) {
            if ((k<nn) && (_heap[j].priority >_heap[k].priority))
                l = k;
            else
                l = j;
            t = _heap[l];
            _heap[l] = _heap[i];
            _heap[i] = t;
            i = l;
            j = 2*i+1;
            k = 2*i+2;
        }
    }

    inline heapitem pop() {
        heapitem it = _heap[0];
        remove();
        return it;
    }
};


/*
 * nodeinfo
 * ========
 */

struct nodeinfo {
    const ckdtreenode  *node;
    npy_intp     m;
    npy_float64  min_distance; /* full min distance */
    npy_float64        buf[1]; // the good old struct hack
    /* accessors to 'packed' attributes */
    inline npy_float64        * const side_distances() {
        /* min distance to the query per side; we
         * update this as the query is proceeded */
        return buf;
    }
    inline npy_float64        * const maxes() {
        return buf + m;
    }
    inline npy_float64        * const mins() {
        return buf + 2 * m;
    }

    inline void init_box(const struct nodeinfo * from) {
        std::memcpy(buf, from->buf, sizeof(npy_float64) * (3 * m));
        min_distance = from->min_distance;
    }

    inline void init_plain(const struct nodeinfo * from) {
        /* skip copying min and max, because we only need side_distance array in this case. */
        std::memcpy(buf, from->buf, sizeof(npy_float64) * m);
        min_distance = from->min_distance;
    }

    inline void update_side_distance(const int d, const npy_float64 new_side_distance, const npy_float64 p) {
        if (NPY_UNLIKELY(ckdtree_isinf(p))) {
            min_distance = dmax(min_distance, new_side_distance);
        } else {
            min_distance += new_side_distance - side_distances()[d];
        }
        side_distances()[d] = new_side_distance;
    }
};

/*
 * Memory pool for nodeinfo structs
 * ================================
 */

struct nodeinfo_pool {

    std::vector<char*> pool;

    npy_intp alloc_size;
    npy_intp arena_size;
    npy_intp m;
    char *arena;
    char *arena_ptr;

    nodeinfo_pool(npy_intp m) {
        alloc_size = sizeof(nodeinfo) + (3 * m -1)*sizeof(npy_float64);
        alloc_size = 64*(alloc_size/64)+64;
        arena_size = 4096*((64*alloc_size)/4096)+4096;
        arena = new char[arena_size];
        arena_ptr = arena;
        pool.push_back(arena);
        this->m = m;
    }

    ~nodeinfo_pool() {
        for (npy_intp i = pool.size()-1; i >= 0; --i)
            delete [] pool[i];
    }

    inline nodeinfo *allocate() {
        nodeinfo *ni1;
        npy_uintp m1 = (npy_uintp)arena_ptr;
        npy_uintp m0 = (npy_uintp)arena;
        if ((arena_size-(npy_intp)(m1-m0))<alloc_size) {
            arena = new char[arena_size];
            arena_ptr = arena;
            pool.push_back(arena);
        }
        ni1 = (nodeinfo*)arena_ptr;
        ni1->m = m;
        arena_ptr += alloc_size;
        return ni1;
    }
};

/* k-nearest neighbor search for a single point x */
template <typename MinMaxDist>
static void
query_single_point(const ckdtree *self,
                   npy_float64   *result_distances,
                   npy_intp      *result_indices,
                   const npy_float64  *x,
                   const npy_intp     *k,
                   const npy_intp     nk,
                   const npy_intp     kmax,
                   const npy_float64  eps,
                   const npy_float64  p,
                   npy_float64  distance_upper_bound)
{
    /* memory pool to allocate and automatically reclaim nodeinfo structs */
    nodeinfo_pool nipool(self->m);

    /*
     * priority queue for chasing nodes
     * entries are:
     *  - minimum distance between the cell and the target
     *  - distances between the nearest side of the cell and the target
     *    the head node of the cell
     */
    heap q(12);

    /*
     *  priority queue for chasing nodes
     *  entries are:
     *   - minimum distance between the cell and the target
     *   - distances between the nearest side of the cell and the target
     *     the head node of the cell
     */
    heap neighbors(kmax);

    npy_intp      i;
    const npy_intp m = self->m;
    nodeinfo      *ni1;
    nodeinfo      *ni2;
    npy_float64   d;
    npy_float64   epsfac;
    heapitem      it, it2, neighbor;
    const ckdtreenode   *node;
    const ckdtreenode   *inode;

    /* set up first nodeifo */
    ni1 = nipool.allocate();
    ni1->node = self->ctree;

    /* initialize first node, update min_distance */
    ni1->min_distance = 0;

    for (i=0; i<m; ++i) {
        ni1->mins()[i] = self->raw_mins[i];
        ni1->maxes()[i] = self->raw_maxes[i];

        npy_float64 side_distance;
        if(self->raw_boxsize_data != NULL) {
            side_distance = BoxDist1D::side_distance_from_min_max(
                self, x[i], self->raw_mins[i], self->raw_maxes[i], i);
        } else {
            side_distance = PlainDist1D::side_distance_from_min_max(
                self, x[i], self->raw_mins[i], self->raw_maxes[i], i);
        }
        side_distance = MinMaxDist::distance_p(side_distance, p);

        ni1->side_distances()[i] = 0;
        ni1->update_side_distance(i, side_distance, p);
    }

    /* fiddle approximation factor */
    if (NPY_LIKELY(p == 2.0)) {
        npy_float64 tmp = 1. + eps;
        epsfac = 1. / (tmp*tmp);
    }
    else if (eps == 0.)
        epsfac = 1.;
    else if (ckdtree_isinf(p))
        epsfac = 1. / (1. + eps);
    else
        epsfac = 1. / std::pow((1. + eps), p);

    /* internally we represent all distances as distance**p */
    if (NPY_LIKELY(p == 2.0)) {
        npy_float64 tmp = distance_upper_bound;
        distance_upper_bound = tmp*tmp;
    }
    else if ((!ckdtree_isinf(p)) && (!ckdtree_isinf(distance_upper_bound)))
        distance_upper_bound = std::pow(distance_upper_bound,p);

    for(;;) {
        if (ni1->node->split_dim == -1) {

            node = ni1->node;

            /* brute-force */
            {
                const npy_intp start_idx = node->start_idx;
                const npy_intp end_idx = node->end_idx;
                const npy_float64 *data = self->raw_data;
                const npy_intp *indices = self->raw_indices;

                prefetch_datapoint(data+indices[start_idx]*m, m);
                if (start_idx < end_idx - 1)
                    prefetch_datapoint(data+indices[start_idx+1]*m, m);

                for (i=start_idx; i<end_idx; ++i) {

                    if (i < end_idx - 2)
                        prefetch_datapoint(data+indices[i+2]*m, m);

                    d = MinMaxDist::point_point_p(self, data+indices[i]*m, x, p, m, distance_upper_bound);
                    if (d < distance_upper_bound) {
                        /* replace furthest neighbor */
                        if (neighbors.n == kmax)
                              neighbors.remove();
                        neighbor.priority = -d;
                        neighbor.contents.intdata = indices[i];
                        neighbors.push(neighbor);

                        /* adjust upper bound for efficiency */
                        if (neighbors.n == kmax)
                            distance_upper_bound = -neighbors.peek().priority;
                    }
                }
            }
            /* done with this node, get another */
            if (q.n == 0) {
                /* no more nodes to visit */
                break;
            }
            else {
                it = q.pop();
                ni1 = (nodeinfo*)(it.contents.ptrdata);
            }

        }
        else {
            inode = ni1->node;
            const npy_intp split_dim = inode->split_dim;
            const npy_float64 split = inode->split;

            /*
             * we don't push cells that are too far onto the queue at all,
             * but since the distance_upper_bound decreases, we might get
             * here even if the cell's too far
             */
            if (ni1->min_distance > distance_upper_bound*epsfac) {
                /* since this is the nearest cell, we're done, bail out */
                break;
            }
            // set up children for searching
            // ni2 will be pushed to the queue

            ni2 = nipool.allocate();

            if (NPY_LIKELY(self->raw_boxsize_data == NULL)) {
                /*
                 * non periodic : the 'near' node is know from the
                 * relative distance to the split, and
                 * has the same distance as the parent node.
                 *
                 * we set ni1 to 'near', and set ni2 to 'far'.
                 * we only recalculate the distance of 'far' later.
                 *
                 * This code branch doesn't use min and max.
                 */
                ni2->init_plain(ni1);

                npy_float64 side_distance;

                if (x[split_dim] < split) {
                    ni1->node = inode->less;
                    ni2->node = inode->greater;
                    side_distance = split - x[split_dim];
                } else {
                    ni1->node = inode->greater;
                    ni2->node = inode->less;
                    side_distance = x[split_dim] - split;
                }

                side_distance = MinMaxDist::distance_p(side_distance, p);

                ni2->update_side_distance(split_dim, side_distance, p);
            } else {
                /*
                 * for periodic queries, we do not know which node is closer.
                 * thus re-calculate ni1 and ni2 both.
                 *
                 * this branch we need to keep track of mins and maxes;
                 */
                ni2->init_box(ni1);

                npy_float64 side_distance;

                ni1->maxes()[split_dim] = split;
                ni1->node = inode->less;

                side_distance = BoxDist1D::side_distance_from_min_max(
                        self,
                        x[split_dim],
                        ni1->mins()[split_dim],
                        ni1->maxes()[split_dim], split_dim);
                side_distance = MinMaxDist::distance_p(side_distance, p);

                ni1->update_side_distance(split_dim, side_distance, p);

                ni2->mins()[split_dim] = split;
                ni2->node = inode->greater;

                side_distance = BoxDist1D::side_distance_from_min_max(
                        self,
                        x[split_dim],
                        ni2->mins()[split_dim],
                        ni2->maxes()[split_dim], split_dim);
                side_distance = MinMaxDist::distance_p(side_distance, p);

                ni2->update_side_distance(split_dim, side_distance, p);
            }

            /* Ensure ni1 is closer than ni2 */
            if (ni1->min_distance > ni2->min_distance) {
                {
                    nodeinfo *tmp;
                    tmp = ni1;
                    ni1 = ni2;
                    ni2 = tmp;
                }
            }

            /*
             * near child is at the same or closer than the distance as the current node
             * we're going here next, so no point pushing it on the queue
             * no need to recompute the distance or the side_distances
             */

            /*
             * far child can be further
             * push it on the queue if it's near enough
             */

            if (ni2->min_distance<=distance_upper_bound*epsfac) {
                it2.priority = ni2->min_distance;
                it2.contents.ptrdata = (void*) ni2;
                q.push(it2);
            }

        }
    }
    /* fill output arrays with sorted neighbors */
    int j = nk - 1;
    while(k[j] > neighbors.n) {
        j--;
    }
    for (i=neighbors.n-1; i>=0; --i) {
        neighbor = neighbors.pop();
        if(i + 1 != k[j]) continue;
        result_indices[j] = neighbor.contents.intdata;
        if (NPY_LIKELY(p == 2.0))
            result_distances[j] = std::sqrt(-neighbor.priority);
        else if ((p == 1.) || (ckdtree_isinf(p)))
            result_distances[j] = -neighbor.priority;
        else
            result_distances[j] = std::pow((-neighbor.priority),(1./p));
        --j;
    }

}

/* Query n points for their k nearest neighbors */

extern "C" PyObject*
query_knn(const ckdtree      *self,
          npy_float64        *dd,
          npy_intp           *ii,
          const npy_float64  *xx,
          const npy_intp     n,
          const npy_intp*     k,
          const npy_intp     nk,
          const npy_intp     kmax,
          const npy_float64  eps,
          const npy_float64  p,
          const npy_float64  distance_upper_bound)
{
#define HANDLE(cond, kls) \
    if(cond) { \
        query_single_point<kls>(self, dd_row, ii_row, xx_row, k, nk, kmax, eps, p, distance_upper_bound); \
    } else

    npy_intp m = self->m;
    npy_intp i;

    /* release the GIL */
    NPY_BEGIN_ALLOW_THREADS
    {
        try {
            if(NPY_LIKELY(!self->raw_boxsize_data)) {
                for (i=0; i<n; ++i) {
                    npy_float64 *dd_row = dd + (i*nk);
                    npy_intp *ii_row = ii + (i*nk);
                    const npy_float64 *xx_row = xx + (i*m);
                    HANDLE(NPY_LIKELY(p == 2), MinkowskiDistP2)
                    HANDLE(p == 1, MinkowskiDistP1)
                    HANDLE(ckdtree_isinf(p), MinkowskiDistPinf)
                    HANDLE(1, MinkowskiDistPp)
                    {}
                }
            } else {
                std::vector<npy_float64> row(m);
                npy_float64 * xx_row = &row[0];
                int j;
                for (i=0; i<n; ++i) {
                    npy_float64 *dd_row = dd + (i*nk);
                    npy_intp *ii_row = ii + (i*nk);
                    const npy_float64 *old_xx_row = xx + (i*m);
                    for(j=0; j<m; ++j) {
                        xx_row[j] = BoxDist1D::wrap_position(old_xx_row[j], self->raw_boxsize_data[j]);
                    }
                    HANDLE(NPY_LIKELY(p == 2), BoxMinkowskiDistP2)
                    HANDLE(p == 1, BoxMinkowskiDistP1)
                    HANDLE(ckdtree_isinf(p), BoxMinkowskiDistPinf)
                    HANDLE(1, BoxMinkowskiDistPp) {}
                }

            }
        }
        catch(...) {
            translate_cpp_exception_with_gil();
        }
    }
    /* reacquire the GIL */
    NPY_END_ALLOW_THREADS

    if (PyErr_Occurred())
        /* true if a C++ exception was translated */
        return NULL;
    else {
        /* return None if there were no errors */
        Py_RETURN_NONE;
    }
}

