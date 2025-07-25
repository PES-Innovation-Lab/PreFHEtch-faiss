/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// -*- c++ -*-

#include <faiss/impl/ProductQuantizer.h>

#include <cstddef>
#include <cstdio>
#include <cstring>
#include <memory>

#include <algorithm>

#include <faiss/IndexFlat.h>
#include <faiss/VectorTransform.h>
#include <faiss/impl/FaissAssert.h>
#include <faiss/utils/distances.h>

extern "C" {

/* declare BLAS functions, see http://www.netlib.org/clapack/cblas/ */

int sgemm_(
        const char* transa,
        const char* transb,
        FINTEGER* m,
        FINTEGER* n,
        FINTEGER* k,
        const float* alpha,
        const float* a,
        FINTEGER* lda,
        const float* b,
        FINTEGER* ldb,
        float* beta,
        float* c,
        FINTEGER* ldc);
}

namespace faiss {

/*********************************************
 * PQ implementation
 *********************************************/

ProductQuantizer::ProductQuantizer(size_t d, size_t M, size_t nbits)
        : Quantizer(d, 0), M(M), nbits(nbits), assign_index(nullptr) {
    set_derived_values();
}

ProductQuantizer::ProductQuantizer() : ProductQuantizer(0, 1, 0) {}

void ProductQuantizer::set_derived_values() {
    // quite a few derived values
    FAISS_THROW_IF_NOT_MSG(
            d % M == 0,
            "The dimension of the vector (d) should be a multiple of the number of subquantizers (M)");
    dsub = d / M;
    code_size = (nbits * M + 7) / 8;
    FAISS_THROW_IF_MSG(nbits > 24, "nbits larger than 24 is not practical.");
    ksub = 1 << nbits;
    centroids.resize(d * ksub);
    verbose = false;
    train_type = Train_default;
}

void ProductQuantizer::set_params(const float* centroids_, int m) {
    memcpy(get_centroids(m, 0),
           centroids_,
           ksub * dsub * sizeof(centroids_[0]));
}

static void init_hypercube(
        int d,
        int nbits,
        int n,
        const float* x,
        float* centroids) {
    std::vector<float> mean(d);
    for (int i = 0; i < n; i++)
        for (int j = 0; j < d; j++)
            mean[j] += x[i * d + j];

    float maxm = 0;
    for (int j = 0; j < d; j++) {
        mean[j] /= n;
        if (fabs(mean[j]) > maxm)
            maxm = fabs(mean[j]);
    }

    for (int i = 0; i < (1 << nbits); i++) {
        float* cent = centroids + i * d;
        for (int j = 0; j < nbits; j++)
            cent[j] = mean[j] + (((i >> j) & 1) ? 1 : -1) * maxm;
        for (int j = nbits; j < d; j++)
            cent[j] = mean[j];
    }
}

static void init_hypercube_pca(
        int d,
        int nbits,
        int n,
        const float* x,
        float* centroids) {
    PCAMatrix pca(d, nbits);
    pca.train(n, x);

    for (int i = 0; i < (1 << nbits); i++) {
        float* cent = centroids + i * d;
        for (int j = 0; j < d; j++) {
            cent[j] = pca.mean[j];
            float f = 1.0;
            for (int k = 0; k < nbits; k++)
                cent[j] += f * sqrt(pca.eigenvalues[k]) *
                        (((i >> k) & 1) ? 1 : -1) * pca.PCAMat[j + k * d];
        }
    }
}

void ProductQuantizer::train(size_t n, const float* x) {
    if (train_type != Train_shared) {
        train_type_t final_train_type;
        final_train_type = train_type;
        if (train_type == Train_hypercube ||
            train_type == Train_hypercube_pca) {
            if (dsub < nbits) {
                final_train_type = Train_default;
                printf("cannot train hypercube: nbits=%zd > log2(d=%zd)\n",
                       nbits,
                       dsub);
            }
        }

        std::unique_ptr<float[]> xslice(new float[n * dsub]);
        for (int m = 0; m < M; m++) {
            for (int j = 0; j < n; j++)
                memcpy(xslice.get() + j * dsub,
                       x + j * d + m * dsub,
                       dsub * sizeof(float));

            Clustering clus(dsub, ksub, cp);

            // we have some initialization for the centroids
            if (final_train_type != Train_default) {
                clus.centroids.resize(dsub * ksub);
            }

            switch (final_train_type) {
                case Train_hypercube:
                    init_hypercube(
                            dsub,
                            nbits,
                            n,
                            xslice.get(),
                            clus.centroids.data());
                    break;
                case Train_hypercube_pca:
                    init_hypercube_pca(
                            dsub,
                            nbits,
                            n,
                            xslice.get(),
                            clus.centroids.data());
                    break;
                case Train_hot_start:
                    memcpy(clus.centroids.data(),
                           get_centroids(m, 0),
                           dsub * ksub * sizeof(float));
                    break;
                default:;
            }

            if (verbose) {
                clus.verbose = true;
                printf("Training PQ slice %d/%zd\n", m, M);
            }
            IndexFlatL2 index(dsub);
            clus.train(n, xslice.get(), assign_index ? *assign_index : index);
            set_params(clus.centroids.data(), m);
        }

    } else {
        Clustering clus(dsub, ksub, cp);

        if (verbose) {
            clus.verbose = true;
            printf("Training all PQ slices at once\n");
        }

        IndexFlatL2 index(dsub);

        clus.train(n * M, x, assign_index ? *assign_index : index);
        for (int m = 0; m < M; m++) {
            set_params(clus.centroids.data(), m);
        }
    }
}

template <class PQEncoder>
void compute_code(const ProductQuantizer& pq, const float* x, uint8_t* code) {
    std::vector<float> distances(pq.ksub);

    // It seems to be meaningless to allocate std::vector<float> distances.
    // But it is done in order to cope the ineffectiveness of the way
    // the compiler generates the code. Basically, doing something like
    //
    //     size_t min_distance = HUGE_VALF;
    //     size_t idxm = 0;
    //     for (size_t i = 0; i < N; i++) {
    //         const float distance = compute_distance(x, y + i * d, d);
    //         if (distance < min_distance) {
    //            min_distance = distance;
    //            idxm = i;
    //         }
    //     }
    //
    // generates significantly more CPU instructions than the baseline
    //
    //     std::vector<float> distances_cached(N);
    //     for (size_t i = 0; i < N; i++) {
    //         distances_cached[i] = compute_distance(x, y + i * d, d);
    //     }
    //     size_t min_distance = HUGE_VALF;
    //     size_t idxm = 0;
    //     for (size_t i = 0; i < N; i++) {
    //         const float distance = distances_cached[i];
    //         if (distance < min_distance) {
    //            min_distance = distance;
    //            idxm = i;
    //         }
    //     }
    //
    // So, the baseline is faster. This is because of the vectorization.
    // I suppose that the branch predictor might affect the performance as well.
    // So, the buffer is allocated, but it might be unused in
    // manually optimized code. Let's hope that the compiler is smart enough to
    // get rid of std::vector allocation in such a case.

    PQEncoder encoder(code, pq.nbits);
    for (size_t m = 0; m < pq.M; m++) {
        const float* xsub = x + m * pq.dsub;

        uint64_t idxm = 0;
        if (pq.transposed_centroids.empty()) {
            // the regular version
            idxm = fvec_L2sqr_ny_nearest(
                    distances.data(),
                    xsub,
                    pq.get_centroids(m, 0),
                    pq.dsub,
                    pq.ksub);
        } else {
            // transposed centroids are available, use'em
            idxm = fvec_L2sqr_ny_nearest_y_transposed(
                    distances.data(),
                    xsub,
                    pq.transposed_centroids.data() + m * pq.ksub,
                    pq.centroids_sq_lengths.data() + m * pq.ksub,
                    pq.dsub,
                    pq.M * pq.ksub,
                    pq.ksub);
        }

        encoder.encode(idxm);
    }
}

void ProductQuantizer::compute_code(const float* x, uint8_t* code) const {
    switch (nbits) {
        case 8:
            faiss::compute_code<PQEncoder8>(*this, x, code);
            break;

        case 16:
            faiss::compute_code<PQEncoder16>(*this, x, code);
            break;

        default:
            faiss::compute_code<PQEncoderGeneric>(*this, x, code);
            break;
    }
}

template <class PQDecoder>
void decode(const ProductQuantizer& pq, const uint8_t* code, float* x) {
    PQDecoder decoder(code, pq.nbits);
    for (size_t m = 0; m < pq.M; m++) {
        uint64_t c = decoder.decode();
        memcpy(x + m * pq.dsub,
               pq.get_centroids(m, c),
               sizeof(float) * pq.dsub);
    }
}

void ProductQuantizer::decode(const uint8_t* code, float* x) const {
    switch (nbits) {
        case 8:
            faiss::decode<PQDecoder8>(*this, code, x);
            break;

        case 16:
            faiss::decode<PQDecoder16>(*this, code, x);
            break;

        default:
            faiss::decode<PQDecoderGeneric>(*this, code, x);
            break;
    }
}

void ProductQuantizer::decode(const uint8_t* code, float* x, size_t n) const {
#pragma omp parallel for if (n > 100)
    for (int64_t i = 0; i < n; i++) {
        this->decode(code + code_size * i, x + d * i);
    }
}

void ProductQuantizer::compute_code_from_distance_table(
        const float* tab,
        uint8_t* code) const {
    PQEncoderGeneric encoder(code, nbits);
    for (size_t m = 0; m < M; m++) {
        float mindis = 1e20;
        uint64_t idxm = 0;

        /* Find best centroid */
        for (size_t j = 0; j < ksub; j++) {
            float dis = *tab++;
            if (dis < mindis) {
                mindis = dis;
                idxm = j;
            }
        }

        encoder.encode(idxm);
    }
}

void ProductQuantizer::compute_codes_with_assign_index(
        const float* x,
        uint8_t* codes,
        size_t n) {
    FAISS_THROW_IF_NOT(assign_index && assign_index->d == dsub);

    for (size_t m = 0; m < M; m++) {
        assign_index->reset();
        assign_index->add(ksub, get_centroids(m, 0));
        size_t bs = 65536;

        std::unique_ptr<float[]> xslice(new float[bs * dsub]);
        std::unique_ptr<idx_t[]> assign(new idx_t[bs]);

        for (size_t i0 = 0; i0 < n; i0 += bs) {
            size_t i1 = std::min(i0 + bs, n);

            for (size_t i = i0; i < i1; i++) {
                memcpy(xslice.get() + (i - i0) * dsub,
                       x + i * d + m * dsub,
                       dsub * sizeof(float));
            }

            assign_index->assign(i1 - i0, xslice.get(), assign.get());

            if (nbits == 8) {
                uint8_t* c = codes + code_size * i0 + m;
                for (size_t i = i0; i < i1; i++) {
                    *c = assign[i - i0];
                    c += M;
                }
            } else if (nbits == 16) {
                uint16_t* c = (uint16_t*)(codes + code_size * i0 + m * 2);
                for (size_t i = i0; i < i1; i++) {
                    *c = assign[i - i0];
                    c += M;
                }
            } else {
                for (size_t i = i0; i < i1; ++i) {
                    uint8_t* c = codes + code_size * i + ((m * nbits) / 8);
                    uint8_t offset = (m * nbits) % 8;
                    uint64_t ass = assign[i - i0];

                    PQEncoderGeneric encoder(c, nbits, offset);
                    encoder.encode(ass);
                }
            }
        }
    }
}

// block size used in ProductQuantizer::compute_codes
int product_quantizer_compute_codes_bs = 256 * 1024;

void ProductQuantizer::compute_codes(const float* x, uint8_t* codes, size_t n)
        const {
    // process by blocks to avoid using too much RAM
    size_t bs = product_quantizer_compute_codes_bs;
    if (n > bs) {
        for (size_t i0 = 0; i0 < n; i0 += bs) {
            size_t i1 = std::min(i0 + bs, n);
            compute_codes(x + d * i0, codes + code_size * i0, i1 - i0);
        }
        return;
    }

    if (dsub < 16) { // simple direct computation

#pragma omp parallel for
        for (int64_t i = 0; i < n; i++)
            compute_code(x + i * d, codes + i * code_size);

    } else { // worthwhile to use BLAS
        std::unique_ptr<float[]> dis_tables(new float[n * ksub * M]);
        compute_distance_tables(n, x, dis_tables.get());

#pragma omp parallel for
        for (int64_t i = 0; i < n; i++) {
            uint8_t* code = codes + i * code_size;
            const float* tab = dis_tables.get() + i * ksub * M;
            compute_code_from_distance_table(tab, code);
        }
    }
}

void ProductQuantizer::compute_distance_table(const float* x, float* dis_table)
        const {
    if (transposed_centroids.empty()) {
        // use regular version
        for (size_t m = 0; m < M; m++) {
            fvec_L2sqr_ny(
                    dis_table + m * ksub,
                    x + m * dsub,
                    get_centroids(m, 0),
                    dsub,
                    ksub);
        }
    } else {
        // transposed centroids are available, use'em
        for (size_t m = 0; m < M; m++) {
            fvec_L2sqr_ny_transposed(
                    dis_table + m * ksub,
                    x + m * dsub,
                    transposed_centroids.data() + m * ksub,
                    centroids_sq_lengths.data() + m * ksub,
                    dsub,
                    M * ksub,
                    ksub);
        }
    }
}

void ProductQuantizer::compute_distance_table_encrypted(
        const float* x,
        float* dis_table) const {
    size_t m;
    for (m = 0; m < M; m++) {
        fvec_L2sqr_ny_encrypted(
                dis_table + m * ksub,
                x + m * dsub,
                get_centroids(m, 0),
                dsub,
                ksub);
    }
}

void ProductQuantizer::compute_inner_prod_table(
        const float* x,
        float* dis_table) const {
    size_t m;

    for (m = 0; m < M; m++) {
        fvec_inner_products_ny(
                dis_table + m * ksub,
                x + m * dsub,
                get_centroids(m, 0),
                dsub,
                ksub);
    }
}

void ProductQuantizer::compute_distance_tables(
        size_t nx,
        const float* x,
        float* dis_tables) const {
#if defined(__AVX2__) || defined(__aarch64__)
    if (dsub == 2 && nbits < 8) { // interesting for a narrow range of settings
        compute_PQ_dis_tables_dsub2(
                d, ksub, centroids.data(), nx, x, false, dis_tables);
    } else
#endif
            if (dsub < 16) {

#pragma omp parallel for if (nx > 1)
        for (int64_t i = 0; i < nx; i++) {
            compute_distance_table(x + i * d, dis_tables + i * ksub * M);
        }

    } else { // use BLAS

        for (int m = 0; m < M; m++) {
            pairwise_L2sqr(
                    dsub,
                    nx,
                    x + dsub * m,
                    ksub,
                    centroids.data() + m * dsub * ksub,
                    dis_tables + ksub * m,
                    d,
                    dsub,
                    ksub * M);
        }
    }
}

void ProductQuantizer::compute_inner_prod_tables(
        size_t nx,
        const float* x,
        float* dis_tables) const {
#if defined(__AVX2__) || defined(__aarch64__)
    if (dsub == 2 && nbits < 8) {
        compute_PQ_dis_tables_dsub2(
                d, ksub, centroids.data(), nx, x, true, dis_tables);
    } else
#endif
            if (dsub < 16) {

#pragma omp parallel for if (nx > 1)
        for (int64_t i = 0; i < nx; i++) {
            compute_inner_prod_table(x + i * d, dis_tables + i * ksub * M);
        }

    } else { // use BLAS

        // compute distance tables
        for (int m = 0; m < M; m++) {
            FINTEGER ldc = ksub * M, nxi = nx, ksubi = ksub, dsubi = dsub,
                     di = d;
            float one = 1.0, zero = 0;

            sgemm_("Transposed",
                   "Not transposed",
                   &ksubi,
                   &nxi,
                   &dsubi,
                   &one,
                   &centroids[m * dsub * ksub],
                   &dsubi,
                   x + dsub * m,
                   &di,
                   &zero,
                   dis_tables + ksub * m,
                   &ldc);
        }
    }
}

/**********************************************
 * Templatized search functions
 * The template class C indicates whether to keep the highest or smallest values
 **********************************************/

namespace {

/* compute an estimator using look-up tables for typical values of M */
template <typename CT, class C>
void pq_estimators_from_tables_Mmul4(
        int M,
        const CT* codes,
        size_t ncodes,
        const float* __restrict dis_table,
        size_t ksub,
        size_t k,
        float* heap_dis,
        int64_t* heap_ids) {
    for (size_t j = 0; j < ncodes; j++) {
        float dis = 0;
        const float* dt = dis_table;

        for (size_t m = 0; m < M; m += 4) {
            float dism = 0;
            dism = dt[*codes++];
            dt += ksub;
            dism += dt[*codes++];
            dt += ksub;
            dism += dt[*codes++];
            dt += ksub;
            dism += dt[*codes++];
            dt += ksub;
            dis += dism;
        }

        if (C::cmp(heap_dis[0], dis)) {
            heap_replace_top<C>(k, heap_dis, heap_ids, dis, j);
        }
    }
}

template <typename CT, class C>
void pq_estimators_from_tables_M4(
        const CT* codes,
        size_t ncodes,
        const float* __restrict dis_table,
        size_t ksub,
        size_t k,
        float* heap_dis,
        int64_t* heap_ids) {
    for (size_t j = 0; j < ncodes; j++) {
        float dis = 0;
        const float* dt = dis_table;
        dis = dt[*codes++];
        dt += ksub;
        dis += dt[*codes++];
        dt += ksub;
        dis += dt[*codes++];
        dt += ksub;
        dis += dt[*codes++];

        if (C::cmp(heap_dis[0], dis)) {
            heap_replace_top<C>(k, heap_dis, heap_ids, dis, j);
        }
    }
}

template <typename CT, class C>
void pq_estimators_from_tables(
        const ProductQuantizer& pq,
        const CT* codes,
        size_t ncodes,
        const float* dis_table,
        size_t k,
        float* heap_dis,
        int64_t* heap_ids) {
    if (pq.M == 4) {
        pq_estimators_from_tables_M4<CT, C>(
                codes, ncodes, dis_table, pq.ksub, k, heap_dis, heap_ids);
        return;
    }

    if (pq.M % 4 == 0) {
        pq_estimators_from_tables_Mmul4<CT, C>(
                pq.M, codes, ncodes, dis_table, pq.ksub, k, heap_dis, heap_ids);
        return;
    }

    /* Default is relatively slow */
    const size_t M = pq.M;
    const size_t ksub = pq.ksub;
    for (size_t j = 0; j < ncodes; j++) {
        float dis = 0;
        const float* __restrict dt = dis_table;
        for (int m = 0; m < M; m++) {
            dis += dt[*codes++];
            dt += ksub;
        }
        if (C::cmp(heap_dis[0], dis)) {
            heap_replace_top<C>(k, heap_dis, heap_ids, dis, j);
        }
    }
}

template <class C>
void pq_estimators_from_tables_generic(
        const ProductQuantizer& pq,
        size_t nbits,
        const uint8_t* codes,
        size_t ncodes,
        const float* dis_table,
        size_t k,
        float* heap_dis,
        int64_t* heap_ids) {
    const size_t M = pq.M;
    const size_t ksub = pq.ksub;
    for (size_t j = 0; j < ncodes; ++j) {
        PQDecoderGeneric decoder(codes + j * pq.code_size, nbits);
        float dis = 0;
        const float* __restrict dt = dis_table;
        for (size_t m = 0; m < M; m++) {
            uint64_t c = decoder.decode();
            dis += dt[c];
            dt += ksub;
        }

        if (C::cmp(heap_dis[0], dis)) {
            heap_replace_top<C>(k, heap_dis, heap_ids, dis, j);
        }
    }
}

template <class C>
void pq_knn_search_with_tables(
        const ProductQuantizer& pq,
        size_t nbits,
        const float* dis_tables,
        const uint8_t* codes,
        const size_t ncodes,
        HeapArray<C>* res,
        bool init_finalize_heap) {
    size_t k = res->k, nx = res->nh;
    size_t ksub = pq.ksub, M = pq.M;

#pragma omp parallel for if (nx > 1)
    for (int64_t i = 0; i < nx; i++) {
        /* query preparation for asymmetric search: compute look-up tables */
        const float* dis_table = dis_tables + i * ksub * M;

        /* Compute distances and keep smallest values */
        int64_t* __restrict heap_ids = res->ids + i * k;
        float* __restrict heap_dis = res->val + i * k;

        if (init_finalize_heap) {
            heap_heapify<C>(k, heap_dis, heap_ids);
        }

        switch (nbits) {
            case 8:
                pq_estimators_from_tables<uint8_t, C>(
                        pq, codes, ncodes, dis_table, k, heap_dis, heap_ids);
                break;

            case 16:
                pq_estimators_from_tables<uint16_t, C>(
                        pq,
                        (uint16_t*)codes,
                        ncodes,
                        dis_table,
                        k,
                        heap_dis,
                        heap_ids);
                break;

            default:
                pq_estimators_from_tables_generic<C>(
                        pq,
                        nbits,
                        codes,
                        ncodes,
                        dis_table,
                        k,
                        heap_dis,
                        heap_ids);
                break;
        }

        if (init_finalize_heap) {
            heap_reorder<C>(k, heap_dis, heap_ids);
        }
    }
}

} // anonymous namespace

void ProductQuantizer::search(
        const float* __restrict x,
        size_t nx,
        const uint8_t* codes,
        const size_t ncodes,
        float_maxheap_array_t* res,
        bool init_finalize_heap) const {
    FAISS_THROW_IF_NOT(nx == res->nh);
    std::unique_ptr<float[]> dis_tables(new float[nx * ksub * M]);
    compute_distance_tables(nx, x, dis_tables.get());

    pq_knn_search_with_tables<CMax<float, int64_t>>(
            *this,
            nbits,
            dis_tables.get(),
            codes,
            ncodes,
            res,
            init_finalize_heap);
}

void ProductQuantizer::search_ip(
        const float* __restrict x,
        size_t nx,
        const uint8_t* codes,
        const size_t ncodes,
        float_minheap_array_t* res,
        bool init_finalize_heap) const {
    FAISS_THROW_IF_NOT(nx == res->nh);
    std::unique_ptr<float[]> dis_tables(new float[nx * ksub * M]);
    compute_inner_prod_tables(nx, x, dis_tables.get());

    pq_knn_search_with_tables<CMin<float, int64_t>>(
            *this,
            nbits,
            dis_tables.get(),
            codes,
            ncodes,
            res,
            init_finalize_heap);
}

void ProductQuantizer::compute_sdc_table() {
    sdc_table.resize(M * ksub * ksub);

    if (dsub < 4) {
#pragma omp parallel for
        for (int mk = 0; mk < M * ksub; mk++) {
            // allow omp to schedule in a more fine-grained way
            // `collapse` is not supported in OpenMP 2.x
            int m = mk / ksub;
            int k = mk % ksub;
            const float* cents = centroids.data() + m * ksub * dsub;
            const float* centi = cents + k * dsub;
            float* dis_tab = sdc_table.data() + m * ksub * ksub;
            fvec_L2sqr_ny(dis_tab + k * ksub, centi, cents, dsub, ksub);
        }
    } else {
        // NOTE: it would disable the omp loop in pairwise_L2sqr
        // but still accelerate especially when M >= 4
#pragma omp parallel for
        for (int m = 0; m < M; m++) {
            const float* cents = centroids.data() + m * ksub * dsub;
            float* dis_tab = sdc_table.data() + m * ksub * ksub;
            pairwise_L2sqr(
                    dsub, ksub, cents, ksub, cents, dis_tab, dsub, dsub, ksub);
        }
    }
}

void ProductQuantizer::search_sdc(
        const uint8_t* qcodes,
        size_t nq,
        const uint8_t* bcodes,
        const size_t nb,
        float_maxheap_array_t* res,
        bool init_finalize_heap) const {
    FAISS_THROW_IF_NOT(sdc_table.size() == M * ksub * ksub);
    FAISS_THROW_IF_NOT(nbits == 8);
    size_t k = res->k;

#pragma omp parallel for
    for (int64_t i = 0; i < nq; i++) {
        /* Compute distances and keep smallest values */
        idx_t* heap_ids = res->ids + i * k;
        float* heap_dis = res->val + i * k;
        const uint8_t* qcode = qcodes + i * code_size;

        if (init_finalize_heap)
            maxheap_heapify(k, heap_dis, heap_ids);

        const uint8_t* bcode = bcodes;
        for (size_t j = 0; j < nb; j++) {
            float dis = 0;
            const float* tab = sdc_table.data();
            for (int m = 0; m < M; m++) {
                dis += tab[bcode[m] + qcode[m] * ksub];
                tab += ksub * ksub;
            }
            if (dis < heap_dis[0]) {
                maxheap_replace_top(k, heap_dis, heap_ids, dis, j);
            }
            bcode += code_size;
        }

        if (init_finalize_heap)
            maxheap_reorder(k, heap_dis, heap_ids);
    }
}

void ProductQuantizer::sync_transposed_centroids() {
    transposed_centroids.resize(d * ksub);
    centroids_sq_lengths.resize(ksub * M);

    for (size_t mi = 0; mi < M; mi++) {
        for (size_t ki = 0; ki < ksub; ki++) {
            float sqlen = 0;

            for (size_t di = 0; di < dsub; di++) {
                const float q = centroids[(mi * ksub + ki) * dsub + di];

                transposed_centroids[(di * M + mi) * ksub + ki] = q;
                sqlen += q * q;
            }

            centroids_sq_lengths[mi * ksub + ki] = sqlen;
        }
    }
}

void ProductQuantizer::clear_transposed_centroids() {
    transposed_centroids.clear();
    transposed_centroids.shrink_to_fit();

    centroids_sq_lengths.clear();
    centroids_sq_lengths.shrink_to_fit();
}

} // namespace faiss
