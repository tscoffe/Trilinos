
#include <Kokkos_Core.hpp>
#include <Kokkos_Atomic.hpp>
#include <impl/Kokkos_Timer.hpp>
#include <Kokkos_MemoryTraits.hpp>
#include <KokkosKernels_SortKeyValue.hpp>
#include <iostream>

#include <Kokkos_UnorderedMap.hpp>

#ifndef _KOKKOSKERNELSUTILS_HPP
#define _KOKKOSKERNELSUTILS_HPP

namespace KokkosKernels{

namespace Experimental{

namespace Util{

template <typename idx, typename ExecutionSpace>
void get_suggested_vector_team_size(
    int max_allowed_team_size,
    int &suggested_vector_size_,
    int &suggested_team_size_,
    idx nr, idx nnz){

#if defined( KOKKOS_HAVE_SERIAL )
  if (Kokkos::Impl::is_same< Kokkos::Serial , ExecutionSpace >::value){
    suggested_vector_size_ =  1;
    suggested_team_size_ = 1;
    return;
  }
#endif

#if defined( KOKKOS_HAVE_PTHREAD )
  if (Kokkos::Impl::is_same< Kokkos::Threads , ExecutionSpace >::value){
    suggested_vector_size_ =  1;
    suggested_team_size_ =  1;
    return;
  }
#endif

#if defined( KOKKOS_HAVE_OPENMP )
  if (Kokkos::Impl::is_same< Kokkos::OpenMP, ExecutionSpace >::value){
    suggested_vector_size_ =  1;
    suggested_team_size_ = 1;
  }
#endif

#if defined( KOKKOS_HAVE_CUDA )
  if (Kokkos::Impl::is_same<Kokkos::Cuda, ExecutionSpace >::value){

    suggested_vector_size_ = nnz / double (nr) + 0.5;

    if (suggested_vector_size_ <= 3){
      suggested_vector_size_ = 2;
    }
    else if (suggested_vector_size_ <= 6){
      suggested_vector_size_ = 4;
    }
    else if (suggested_vector_size_ <= 12){
      suggested_vector_size_ = 8;
    }
    else if (suggested_vector_size_ <= 24){
      suggested_vector_size_ = 16;
    }
    else {
      suggested_vector_size_ = 32;
    }
    if (max_allowed_team_size < 32){
      std::cerr << "max_allowed_team_size:" << max_allowed_team_size << std::endl;
    }

    suggested_team_size_ = max_allowed_team_size / suggested_vector_size_;
  }
#endif

#if defined( KOKKOS_HAVE_QTHREAD)
  if (Kokkos::Impl::is_same< Kokkos::Qthread, ExecutionSpace >::value){
    suggested_vector_size_ = 1;
    suggested_team_size_ = 1;
  }
#endif

}


template <typename idx_array_type,
          typename idx_edge_array_type,
          typename idx_out_edge_array_type,
          typename team_member>
struct FillSymmetricEdges{
  typedef typename idx_array_type::value_type idx;
  idx num_rows;
  idx nnz;
  idx_array_type xadj;
  idx_edge_array_type adj;

  idx_out_edge_array_type srcs;
  idx_out_edge_array_type dsts;

  FillSymmetricEdges(
    typename idx_array_type::value_type num_rows_,
    idx_array_type xadj_,
    idx_edge_array_type adj_,

    idx_out_edge_array_type srcs_,
    idx_out_edge_array_type dsts_
    ):num_rows(num_rows_),nnz(adj_.dimension_0()), xadj(xadj_), adj(adj_), srcs(srcs_), dsts(dsts_){}

  KOKKOS_INLINE_FUNCTION
  void operator()(const team_member & teamMember) const {
    idx ii = teamMember.league_rank()  * teamMember.team_size()+ teamMember.team_rank();
    if (ii >= num_rows) return;
    idx row_begin = xadj[ii];
    idx row_end = xadj[ii + 1];

    Kokkos::parallel_for(
        Kokkos::ThreadVectorRange(teamMember, row_end - row_begin),
        [&] (idx i) {
      idx adjind = i + row_begin;
      idx colIndex = adj[adjind];
      if (colIndex < num_rows){
        srcs[adjind] = ii + 1;
        dsts[adjind] = colIndex + 1;
        if (colIndex != ii){
          srcs[adjind + nnz] = colIndex + 1;
          dsts[adjind + nnz] = ii + 1;
        }
      }

    });

  }
};


template <typename in_lno_row_view_t,
          typename in_lno_nnz_view_t,
          typename hashmap_t,
          typename out_lno_row_view_t,
          typename team_member>
struct FillSymmetricEdgesHashMap{
  typedef typename in_lno_row_view_t::value_type idx;
  idx num_rows;
  idx nnz;
  in_lno_row_view_t xadj;
  in_lno_nnz_view_t adj;
  hashmap_t umap;
  out_lno_row_view_t pre_pps;
  bool lower_only;

  FillSymmetricEdgesHashMap(
      idx num_rows_,
    in_lno_row_view_t xadj_,
    in_lno_nnz_view_t adj_,
    hashmap_t hashmap_,
    out_lno_row_view_t pre_pps_
    ):num_rows(num_rows_),nnz(adj_.dimension_0()), xadj(xadj_), adj(adj_),
        umap(hashmap_), pre_pps(pre_pps_){}

  KOKKOS_INLINE_FUNCTION
  void operator()(const team_member & teamMember/*, idx &nnz*/) const {
    idx ii = teamMember.league_rank()  * teamMember.team_size()+ teamMember.team_rank();
    if (ii >= num_rows) {
      return;
    }
    idx row_begin = xadj[ii];
    idx row_end = xadj[ii + 1];
    Kokkos::parallel_for(
        Kokkos::ThreadVectorRange(teamMember, row_end - row_begin),
        [&] (idx i) {
      idx adjind = i + row_begin;
      idx colIndex = adj[adjind];
      if (colIndex < num_rows){
        if (colIndex < ii){
          Kokkos::UnorderedMapInsertResult r = umap.insert(Kokkos::pair<idx, idx>(colIndex, ii));
          if (r.success()){

            Kokkos::atomic_fetch_add(&(pre_pps(ii)),1);

            Kokkos::atomic_fetch_add(&(pre_pps(colIndex)),1);
          }
        }
        else if (colIndex > ii){

          Kokkos::UnorderedMapInsertResult r = umap.insert(Kokkos::pair<idx, idx>(ii, colIndex));
          if (r.success()){
            Kokkos::atomic_fetch_add(&(pre_pps(colIndex)),1);

            Kokkos::atomic_fetch_add(&(pre_pps(ii)),1);
          }
        }
        else {
          Kokkos::atomic_fetch_add(&(pre_pps(ii)),1);
        }
      }

    });

  }
};

template <typename in_lno_row_view_t,
          typename in_lno_nnz_view_t,
          typename hashmap_t,
          typename out_lno_row_view_t,
          typename team_member>
struct FillSymmetricLowerEdgesHashMap{
  typedef typename in_lno_row_view_t::value_type idx;
  idx num_rows;
  idx nnz;
  in_lno_row_view_t xadj;
  in_lno_nnz_view_t adj;
  hashmap_t umap;
  out_lno_row_view_t pre_pps;


  FillSymmetricLowerEdgesHashMap(
      idx num_rows_,
    in_lno_row_view_t xadj_,
    in_lno_nnz_view_t adj_,
    hashmap_t hashmap_,
    out_lno_row_view_t pre_pps_,
    bool lower_only_ = false
    ):num_rows(num_rows_),nnz(adj_.dimension_0()), xadj(xadj_), adj(adj_),
        umap(hashmap_), pre_pps(pre_pps_){}

  KOKKOS_INLINE_FUNCTION
  void operator()(const team_member & teamMember/*, idx &nnz*/) const {
    idx ii = teamMember.league_rank()  * teamMember.team_size()+ teamMember.team_rank();
    if (ii >= num_rows) {
      return;
    }
    idx row_begin = xadj[ii];
    idx row_end = xadj[ii + 1];

    Kokkos::parallel_for(
        Kokkos::ThreadVectorRange(teamMember, row_end - row_begin),
        [&] (idx i) {
      idx adjind = i + row_begin;
      idx colIndex = adj[adjind];
      if (colIndex < num_rows){
        if (colIndex < ii){
          Kokkos::UnorderedMapInsertResult r = umap.insert(Kokkos::pair<idx, idx>(colIndex, ii));
          if (r.success()){

            Kokkos::atomic_fetch_add(&(pre_pps(colIndex)),1);
          }
        }
        else if (colIndex > ii){

          Kokkos::UnorderedMapInsertResult r = umap.insert(Kokkos::pair<idx, idx>(ii, colIndex));
          if (r.success()){
            Kokkos::atomic_fetch_add(&(pre_pps(ii)),1);
          }
        }

      }

    });
  }
};

template <typename in_lno_row_view_t,
          typename in_lno_nnz_view_t,
          typename hashmap_t,
          typename out_lno_row_view_t,
          typename out_lno_nnz_view_t,
          typename team_member_t>
struct FillSymmetricCRS_HashMap{
  typedef typename in_lno_row_view_t::value_type idx;
  idx num_rows;
  idx nnz;
  in_lno_row_view_t xadj;
  in_lno_nnz_view_t adj;
  hashmap_t umap;
  out_lno_row_view_t pre_pps;
  out_lno_nnz_view_t sym_adj;

  FillSymmetricCRS_HashMap(idx num_rows_,
        in_lno_row_view_t xadj_,
        in_lno_nnz_view_t adj_,
        hashmap_t hashmap_,
        out_lno_row_view_t pre_pps_,
        out_lno_nnz_view_t sym_adj_):
            num_rows(num_rows_),nnz(adj_.dimension_0()),
      xadj(xadj_), adj(adj_),
      umap(hashmap_), pre_pps(pre_pps_), sym_adj(sym_adj_){}

  KOKKOS_INLINE_FUNCTION
  void operator()(const team_member_t & teamMember) const {
    idx ii = teamMember.league_rank()  * teamMember.team_size()+ teamMember.team_rank();
    if (ii >= num_rows) {
      return;
    }
    idx row_begin = xadj[ii];
    idx row_end = xadj[ii + 1];

    Kokkos::parallel_for(
        Kokkos::ThreadVectorRange(teamMember, row_end - row_begin),
        [&] (idx i) {
      idx adjind = i + row_begin;
      idx colIndex = adj[adjind];
      if (colIndex < num_rows){
        if (colIndex < ii){
          if (umap.insert(Kokkos::pair<idx, idx>(colIndex, ii)).success()){
            idx cAdjInd = Kokkos::atomic_fetch_add(&(pre_pps(colIndex)),1);
            idx iAdjInd = Kokkos::atomic_fetch_add(&(pre_pps(ii)),1);
            sym_adj[cAdjInd] = ii;
            sym_adj[iAdjInd] = colIndex;
          }
        }
        else if (colIndex > ii){
          if (umap.insert(Kokkos::pair<idx, idx>(ii, colIndex)).success()){
            idx cAdjInd = Kokkos::atomic_fetch_add(&(pre_pps(colIndex)),1);
            idx iAdjInd = Kokkos::atomic_fetch_add(&(pre_pps(ii)),1);
            sym_adj[cAdjInd] = ii;
            sym_adj[iAdjInd] = colIndex;
          }
        }
        else {
          idx cAdjInd = Kokkos::atomic_fetch_add(&(pre_pps(colIndex)),1);
          sym_adj[cAdjInd] = ii;
        }
      }
    });

  }
};


template <typename in_lno_row_view_t,
          typename in_lno_nnz_view_t,
          typename hashmap_t,
          typename out_lno_nnz_view_t,
          typename out_lno_row_view_t,
          typename team_member_t>
struct FillSymmetricEdgeList_HashMap{
  typedef typename in_lno_row_view_t::value_type idx;
  idx num_rows;
  idx nnz;
  in_lno_row_view_t xadj;
  in_lno_nnz_view_t adj;
  hashmap_t umap;
  out_lno_nnz_view_t sym_src;
  out_lno_nnz_view_t sym_dst;
  out_lno_row_view_t pps;

  FillSymmetricEdgeList_HashMap(
      idx num_rows_,
        in_lno_row_view_t xadj_,
        in_lno_nnz_view_t adj_,
        hashmap_t hashmap_,
        out_lno_nnz_view_t sym_src_,
        out_lno_nnz_view_t sym_dst_,
        out_lno_row_view_t pps_):
            num_rows(num_rows_),nnz(adj_.dimension_0()),
      xadj(xadj_), adj(adj_),
      umap(hashmap_), sym_src(sym_src_), sym_dst(sym_dst_), pps(pps_){}

  KOKKOS_INLINE_FUNCTION
  void operator()(const team_member_t & teamMember) const {
    idx ii = teamMember.league_rank()  * teamMember.team_size()+ teamMember.team_rank();
    if (ii >= num_rows) {
      return;
    }
    idx row_begin = xadj[ii];
    idx row_end = xadj[ii + 1];

    Kokkos::parallel_for(
        Kokkos::ThreadVectorRange(teamMember, row_end - row_begin),
        [&] (idx i) {
      idx adjind = i + row_begin;
      idx colIndex = adj[adjind];
      if (colIndex < num_rows){
        if (colIndex < ii){
          if (umap.insert(Kokkos::pair<idx, idx>(colIndex, ii)).success()){
            idx cAdjInd = Kokkos::atomic_fetch_add(&(pps(colIndex)),1);
            sym_src[cAdjInd] = colIndex;
            sym_dst[cAdjInd] = ii;
          }
        }
        else if (colIndex > ii){
          if (umap.insert(Kokkos::pair<idx, idx>(ii, colIndex)).success()){
            idx cAdjInd = Kokkos::atomic_fetch_add(&(pps(ii)),1);
            sym_src[cAdjInd] = ii;
            sym_dst[cAdjInd] = colIndex;
          }
        }
      }
    });

  }
};
template <typename idx_array_type>
void print_1Dview(idx_array_type view, bool print_all = false){

  typedef typename idx_array_type::HostMirror host_type;
  typedef typename idx_array_type::size_type idx;
  host_type host_view = Kokkos::create_mirror_view (view);
  Kokkos::deep_copy (host_view , view);
  idx nr = host_view.dimension_0();
  if (!print_all){


    if (nr > 20){
      idx n = 10;
      for (idx i = 0; i < n; ++i){
        std::cout << host_view(i) << " ";
      }
      std::cout << "... ... ... ";

      for (idx i = nr-n; i < nr; ++i){
        std::cout << host_view(i) << " ";
      }
      std::cout << std::endl;
    }
    else {
      for (idx i = 0; i < nr; ++i){
        std::cout << host_view(i) << " ";
      }
      std::cout << std::endl;
    }
  }
  else {
    for (idx i = 0; i < nr; ++i){
      std::cout << host_view(i) << " ";
    }
    std::cout << std::endl;
  }
}

template <typename forward_map_type, typename reverse_map_type>
struct Reverse_Map_Init{
  typedef typename forward_map_type::value_type forward_type;
  typedef typename reverse_map_type::value_type reverse_type;
  forward_map_type forward_map;
  reverse_map_type reverse_map_xadj;
  Reverse_Map_Init(
      forward_map_type forward_map_,
      reverse_map_type reverse_xadj_):
        forward_map(forward_map_), reverse_map_xadj(reverse_xadj_){}

  KOKKOS_INLINE_FUNCTION
  void operator()(const size_t &ii) const {
    forward_type fm = forward_map[ii];
    Kokkos::atomic_fetch_add( &(reverse_map_xadj(fm)), 1);
  }

  /*
  KOKKOS_INLINE_FUNCTION
  void operator()(const forward_type ii, size_t& update, const bool final) const {
    update += reverse_map_xadj(ii);
    if (final) {
      reverse_map_xadj(ii) = reverse_type (update);
    }
  }
  */
};








template <typename forward_map_type, typename reverse_map_type>
struct Fill_Reverse_Map{
  typedef typename forward_map_type::value_type forward_type;
  typedef typename reverse_map_type::value_type reverse_type;
  forward_map_type forward_map;
  reverse_map_type reverse_map_xadj;
  reverse_map_type reverse_map_adj;


  Fill_Reverse_Map(
      forward_map_type forward_map_,
      reverse_map_type reverse_map_xadj_,
      reverse_map_type reverse_map_adj):
        forward_map(forward_map_), reverse_map_xadj(reverse_map_xadj_), reverse_map_adj(reverse_map_adj){}

  KOKKOS_INLINE_FUNCTION
  void operator()(const size_t &ii) const {
    forward_type c = forward_map[ii];
    const reverse_type future_index = Kokkos::atomic_fetch_add( &(reverse_map_xadj(c - 1)), 1);
    reverse_map_adj(future_index) = ii;
  }
};


template <typename array_type>
struct InclusiveParallelPrefixSum{
  typedef typename array_type::value_type idx;
  array_type array_sum;
  InclusiveParallelPrefixSum(array_type arr_): array_sum(arr_){}

  KOKKOS_INLINE_FUNCTION
  void operator()(const size_t ii, size_t& update, const bool final) const {
    update += array_sum(ii);
    if (final) {
      array_sum(ii) = idx (update);
    }
  }
};

template <typename forward_array_type, typename MyExecSpace>
void inclusive_parallel_prefix_sum(typename forward_array_type::value_type num_elements, forward_array_type arr){
  typedef Kokkos::RangePolicy<MyExecSpace> my_exec_space;
  Kokkos::parallel_scan( my_exec_space(0, num_elements), InclusiveParallelPrefixSum<forward_array_type>(arr));
}

template <typename array_type>
struct ExclusiveParallelPrefixSum{
  typedef typename array_type::value_type idx;
  array_type array_sum;
  ExclusiveParallelPrefixSum(array_type arr_): array_sum(arr_){}

  KOKKOS_INLINE_FUNCTION
  void operator()(const size_t ii, size_t& update, const bool final) const {

    idx val = array_sum(ii);
    if (final) {
      array_sum(ii) = idx (update);
    }
    update += val;
  }
};

template <typename forward_array_type, typename MyExecSpace>
void exclusive_parallel_prefix_sum(typename forward_array_type::value_type num_elements, forward_array_type arr){
  typedef Kokkos::RangePolicy<MyExecSpace> my_exec_space;
  Kokkos::parallel_scan( my_exec_space(0, num_elements), ExclusiveParallelPrefixSum<forward_array_type>(arr));
}

template <typename array_type>
struct PropogataMaxValstoZeros{
  typedef typename array_type::value_type idx;
  array_type array_sum;
  PropogataMaxValstoZeros(array_type arr_): array_sum(arr_){}

  KOKKOS_INLINE_FUNCTION
  void operator()(const size_t ii, idx& update, const bool final) const {

    idx value = array_sum(ii);
    if (value != 0) {
      update = value;
    }
    else if (final ){
      array_sum(ii) = idx (update);
    }
  }

  KOKKOS_INLINE_FUNCTION
  void join( volatile       idx & update
           , volatile const idx & input ) const {
    if (input > update) update = input;
  }


};


template <typename array_type>
struct LinearInitialization{
  typedef typename array_type::value_type idx;
  array_type array_sum;
  LinearInitialization(array_type arr_): array_sum(arr_){}

  KOKKOS_INLINE_FUNCTION
  void operator()(const size_t ii) const {
    array_sum(ii) = ii;
  }
};
template <typename array_type, typename MyExecSpace>
void linear_init(typename array_type::value_type num_elements, array_type arr){
  typedef Kokkos::RangePolicy<MyExecSpace> my_exec_space;
  Kokkos::parallel_for( my_exec_space(0, num_elements), LinearInitialization<array_type>(arr));
}


template <typename forward_array_type, typename MyExecSpace>
void remove_zeros_in_xadj_vector(typename forward_array_type::value_type num_elements, forward_array_type arr){
  typedef Kokkos::RangePolicy<MyExecSpace> my_exec_space;
  Kokkos::parallel_scan( my_exec_space(0, num_elements), PropogataMaxValstoZeros<forward_array_type>(arr));
}


template <typename forward_array_type, typename reverse_array_type>
struct FillReverseBegins{

  const forward_array_type &forward_map; //vertex to colors
  reverse_array_type &reverse_map_xadj; // colors to vertex xadj


  FillReverseBegins(
      const forward_array_type &forward_map_, //vertex to colors
      reverse_array_type &reverse_map_xadj_ // colors to vertex xadj
      ):
        forward_map(forward_map_), reverse_map_xadj(reverse_map_xadj_){}

  KOKKOS_INLINE_FUNCTION
  void operator()(const size_t ii) const {
    typename forward_array_type::value_type prev_col = forward_map(ii - 1);
    typename forward_array_type::value_type cur_col = forward_map(ii);
    while (prev_col < cur_col){
      prev_col += 1;
      forward_map(prev_col) = ii + 1;
    }
  }

};


template <typename forward_map_type, typename reverse_map_type>
struct Reverse_Map_Scale_Init{
  typedef typename forward_map_type::value_type forward_type;
  typedef typename reverse_map_type::value_type reverse_type;
  forward_map_type forward_map;
  reverse_map_type reverse_map_xadj;


  const reverse_type multiply_shift_for_scale;
  const reverse_type division_shift_for_bucket;

  Reverse_Map_Scale_Init(
      forward_map_type forward_map_,
      reverse_map_type reverse_xadj_,
      reverse_type multiply_shift_for_scale_,
      reverse_type division_shift_for_bucket_):
        forward_map(forward_map_), reverse_map_xadj(reverse_xadj_),
        multiply_shift_for_scale(multiply_shift_for_scale_),
        division_shift_for_bucket(division_shift_for_bucket_){}

  KOKKOS_INLINE_FUNCTION
  void operator()(const size_t &ii) const {
    forward_type fm = forward_map[ii];
    fm = fm << multiply_shift_for_scale;
    fm += ii >> division_shift_for_bucket;
    Kokkos::atomic_fetch_add( &(reverse_map_xadj(fm)), 1);
  }
};



template <typename forward_map_type, typename reverse_map_type>
struct Fill_Reverse_Scale_Map{
  typedef typename forward_map_type::value_type forward_type;
  typedef typename reverse_map_type::value_type reverse_type;
  forward_map_type forward_map;
  reverse_map_type reverse_map_xadj;
  reverse_map_type reverse_map_adj;

  const reverse_type multiply_shift_for_scale;
  const reverse_type division_shift_for_bucket;


  Fill_Reverse_Scale_Map(
      forward_map_type forward_map_,
      reverse_map_type reverse_map_xadj_,
      reverse_map_type reverse_map_adj_,
      reverse_type multiply_shift_for_scale_,
      reverse_type division_shift_for_bucket_):
        forward_map(forward_map_), reverse_map_xadj(reverse_map_xadj_), reverse_map_adj(reverse_map_adj_),
        multiply_shift_for_scale(multiply_shift_for_scale_),
        division_shift_for_bucket(division_shift_for_bucket_){}

  KOKKOS_INLINE_FUNCTION
  void operator()(const size_t &ii) const {
    forward_type fm = forward_map[ii];

    fm = fm << multiply_shift_for_scale;
    fm += ii >> division_shift_for_bucket;
    const reverse_type future_index = Kokkos::atomic_fetch_add( &(reverse_map_xadj(fm - 1)), 1);
    reverse_map_adj(future_index) = ii;
  }
};

template <typename from_view_t, typename to_view_t>
struct StridedCopy{
  const from_view_t from;
  to_view_t to;
  const size_t stride;
  StridedCopy(
      const from_view_t from_,
      to_view_t to_,
      size_t stride_):from(from_), to (to_), stride(stride_){}


  KOKKOS_INLINE_FUNCTION
  void operator()(const size_t &ii) const {
    //std::cout << "ii:" << ii << " ii * stride:" << ii * stride << std::endl;
    to[ii] = from[(ii + 1) * stride - 1];
  }
};

/**
 * \brief Utility function to obtain a reverse map given a map.
 * Input is a map with the number of elements within the map.
 * forward_map[c] = i, where c is a forward elements and forward_map has a size of num_forward_elements.
 * i is the value that c is mapped in the forward map, and the range of that is num_reverse_elements.
 * Output is the reverse_map_xadj and reverse_map_adj such that,
 * all c, forward_map[c] = i, will appear in  reverse_map_adj[ reverse_map_xadj[i]: reverse_map_xadj[i+1])
 * \param: num_forward_elements: the number of elements in the forward map, the size of the forward map.
 * \param: num_reverse_elements: the number of elements that forward map is mapped to. It is the value of max i.
 * \param: forward_map: input forward_map, where forward_map[c] = i.
 * \param: reverse_map_xadj: reverse map xadj, that is it will hold the beginning and
 * end indices on reverse_map_adj such that all values mapped to i will be [ reverse_map_xadj[i]: reverse_map_xadj[i+1])
 * its size will be num_reverse_elements + 1.
 * \param: reverse_map_adj: reverse map adj, holds the values of reverse maps. Its size will be num_forward_elements.
 *
 */
template <typename forward_array_type, typename reverse_array_type, typename MyExecSpace>
void create_reverse_map(
    const typename reverse_array_type::value_type &num_forward_elements, //num_vertices
    const typename forward_array_type::value_type &num_reverse_elements, //num_colors

    const forward_array_type &forward_map, //vertex to colors
    reverse_array_type &reverse_map_xadj, // colors to vertex xadj
    reverse_array_type &reverse_map_adj){ //colros to vertex adj

  typedef typename reverse_array_type::value_type lno_t;
  typedef typename forward_array_type::value_type reverse_lno_t;

  const lno_t  MINIMUM_TO_ATOMIC = 64;



  typedef Kokkos::RangePolicy<MyExecSpace> my_exec_space;
  reverse_map_xadj = reverse_array_type("Reverse Map Xadj", num_reverse_elements + 1);
  reverse_map_adj = reverse_array_type(Kokkos::ViewAllocateWithoutInitializing("REVERSE_ADJ"), num_forward_elements);



  if (num_reverse_elements < MINIMUM_TO_ATOMIC){
    const lno_t  scale_size = 1024;
    const lno_t  multiply_shift_for_scale = 10;
    const lno_t division_shift_for_bucket = lno_t (ceil(log(num_forward_elements / scale_size)/log(2)));
    const lno_t bucket_range_size = pow(2, division_shift_for_bucket);

    //coloring indices are base-1. we end up using not using element 1.
    const reverse_lno_t tmp_reverse_size = (num_reverse_elements + 1) << multiply_shift_for_scale;

    reverse_array_type tmp_color_xadj ("TMP_REVERSE_XADJ",
        tmp_reverse_size + 1);

    Reverse_Map_Scale_Init<forward_array_type, reverse_array_type> rmi(
        forward_map,
        tmp_color_xadj,
        multiply_shift_for_scale,
        division_shift_for_bucket);
#ifdef KOKKOSKERNELS_TIME_REVERSE
    Kokkos::Impl::Timer timer;
#endif
    Kokkos::parallel_for (my_exec_space (0, num_forward_elements) , rmi);
    MyExecSpace::fence();
#ifdef KOKKOSKERNELS_TIME_REVERSE
    std::cout << "-CREATE_INIT_REVERSE_MAP:" << timer.seconds() << std::endl;
    timer.reset();
#endif
    //print_1Dview(tmp_color_xadj, true);


    inclusive_parallel_prefix_sum<reverse_array_type, MyExecSpace>(tmp_reverse_size + 1, tmp_color_xadj);
    MyExecSpace::fence();
#ifdef KOKKOSKERNELS_TIME_REVERSE
    std::cout << "-CREATE_SCAN_REVERSE_MAP:" << timer.seconds() << std::endl;
    //print_1Dview(tmp_color_xadj, true);
#endif

    Kokkos::parallel_for (my_exec_space (0, num_reverse_elements + 1) , StridedCopy<reverse_array_type, reverse_array_type>(tmp_color_xadj, reverse_map_xadj, scale_size));
    MyExecSpace::fence();
#ifdef KOKKOSKERNELS_TIME_REVERSE
    //print_1Dview(tmp_color_xadj, true);
    //print_1Dview(reverse_map_xadj,true);
    timer.reset();
#endif
    Fill_Reverse_Scale_Map<forward_array_type, reverse_array_type> frm (forward_map, tmp_color_xadj, reverse_map_adj,
        multiply_shift_for_scale, division_shift_for_bucket);
    Kokkos::parallel_for (my_exec_space (0, num_forward_elements) , frm);
    MyExecSpace::fence();
#ifdef KOKKOSKERNELS_TIME_REVERSE
    std::cout << "-CREATE_FILL_REVERSE_MAP:" << timer.seconds() << std::endl;
    //print_1Dview(reverse_map_adj);
#endif
  }
  else
  //atomic implementation.
  {
    reverse_array_type tmp_color_xadj (Kokkos::ViewAllocateWithoutInitializing("TMP_REVERSE_XADJ"), num_reverse_elements + 1);

    Reverse_Map_Init<forward_array_type, reverse_array_type> rmi(forward_map, reverse_map_xadj);
#ifdef KOKKOSKERNELS_TIME_REVERSE
    Kokkos::Impl::Timer timer;
#endif

    Kokkos::parallel_for (my_exec_space (0, num_forward_elements) , rmi);
    MyExecSpace::fence();
    //print_1Dview(reverse_map_xadj);
#ifdef KOKKOSKERNELS_TIME_REVERSE
    std::cout << "CREATE_INIT_REVERSE_MAP:" << timer.seconds() << std::endl;
    timer.reset();
#endif


    inclusive_parallel_prefix_sum<reverse_array_type, MyExecSpace>(num_reverse_elements + 1, reverse_map_xadj);
    MyExecSpace::fence();
#ifdef KOKKOSKERNELS_TIME_REVERSE
    //print_1Dview(reverse_map_xadj);
    std::cout << "CREATE_SCAN_REVERSE_MAP:" << timer.seconds() << std::endl;
#endif
    Kokkos::deep_copy (tmp_color_xadj, reverse_map_xadj);
    MyExecSpace::fence();
#ifdef KOKKOSKERNELS_TIME_REVERSE
    timer.reset();
#endif
    Fill_Reverse_Map<forward_array_type, reverse_array_type> frm (forward_map, tmp_color_xadj, reverse_map_adj);
    Kokkos::parallel_for (my_exec_space (0, num_forward_elements) , frm);
    MyExecSpace::fence();
#ifdef KOKKOSKERNELS_TIME_REVERSE
    std::cout << "CREATE_FILL_REVERSE_MAP:" << timer.seconds() << std::endl;
#endif
  }
}


template <typename value_array_type, typename out_value_array_type, typename idx_array_type>
struct PermuteVector{
  typedef typename idx_array_type::value_type idx;
  value_array_type old_vector;
  out_value_array_type new_vector;
  idx_array_type old_to_new_mapping;
  idx mapping_size;
  PermuteVector(
      value_array_type old_vector_,
      out_value_array_type new_vector_,
      idx_array_type old_to_new_mapping_):
        old_vector(old_vector_), new_vector(new_vector_),old_to_new_mapping(old_to_new_mapping_), mapping_size(old_to_new_mapping_.dimension_0()){}

  KOKKOS_INLINE_FUNCTION
  void operator()(const size_t &ii) const {

    idx mapping = ii;
    if (ii < mapping_size) mapping = old_to_new_mapping[ii];
    new_vector[mapping] = old_vector[ii];
  }
};

template <typename value_array_type, typename out_value_array_type, typename idx_array_type, typename MyExecSpace>
void permute_vector(
    typename idx_array_type::value_type num_elements,
    idx_array_type &old_to_new_index_map,
    value_array_type &old_vector,
    out_value_array_type &new_vector
    ){
  typedef Kokkos::RangePolicy<MyExecSpace> my_exec_space;

  Kokkos::parallel_for( my_exec_space(0,num_elements),
      PermuteVector<value_array_type, out_value_array_type, idx_array_type>(old_vector, new_vector, old_to_new_index_map));

}

template <typename value_array_type>
struct ZeroVector{
  value_array_type myview;
  ZeroVector(value_array_type myview_): myview(myview_){}

  KOKKOS_INLINE_FUNCTION
  void operator()( const size_t i ) const {
    myview(i) = 0;
  }
};

template <typename value_array_type, typename MyExecSpace>
void zero_vector(
    typename value_array_type::value_type num_elements,
    value_array_type &vector
    ){
  typedef Kokkos::RangePolicy<MyExecSpace> my_exec_space;
  Kokkos::parallel_for( my_exec_space(0,num_elements), ZeroVector<value_array_type>(vector));

}


template <typename v1, typename v2, typename v3>
struct MarkDuplicateSortedKeyValuePairs{
  v1 keys;
  v2 vals;
  v3 prefix_sum;
  typename v1::size_type overall_size;
  MarkDuplicateSortedKeyValuePairs(v1 keys_,v2 vals_, v3 prefix_sum_, typename v1::size_type overall_size_):
    keys(keys_), vals(vals_), prefix_sum(prefix_sum_), overall_size(overall_size_){}

  KOKKOS_INLINE_FUNCTION
  void operator()(const size_t &i, typename v3::value_type &num_result) const {
    typename v1::value_type my_key = keys(i);
    typename v2::value_type my_val = vals(i);

    if ((my_key != 0 && my_val != 0) && ((i + 1 >= overall_size) || (my_key != keys(i + 1) || my_val != vals(i + 1)))){
      prefix_sum(i) = 1;
      num_result += 1;
    }
  }


};

template <typename v1, typename v2, typename v3, typename v4, typename v5>
struct FillSymmetricCSR{
  v1 keys;
  v2 vals;
  v3 prefix_sum;
  typename v3::size_type array_size;
  v4 out_xadj;
  v5 out_adj;
  FillSymmetricCSR(
      v1 keys_,v2 vals_, v3 prefix_sum_, typename v3::size_type array_size_,
      v4 out_xadj_, v5 out_adj_):
        keys(keys_), vals(vals_), prefix_sum(prefix_sum_), array_size(array_size_),
        out_xadj(out_xadj_), out_adj(out_adj_){}

  KOKKOS_INLINE_FUNCTION
  void operator()(const size_t &i) const {
    typename v3::value_type my_pos = prefix_sum(i);

    if (i + 1 >= array_size){
      typename v2::value_type my_val = vals(i);
      typename v1::value_type my_key = keys(i);
      out_adj(my_pos) = my_val - 1;
      out_xadj(my_key) = my_pos + 1;
    }
    else {
      typename v3::value_type next_pos = prefix_sum(i + 1);
      if (my_pos != next_pos){

        typename v2::value_type my_val = vals(i);
        typename v1::value_type my_key = keys(i);
        typename v1::value_type next_key = keys(i + 1);
        out_adj(my_pos) = my_val - 1;
        if (my_key != next_key){
          out_xadj(my_key) = my_pos + 1;

        }

      }
    }
  }


};



template <typename in_lno_row_view_t,
          typename in_lno_nnz_view_t,
          typename out_lno_nnz_view_t,
          typename MyExecSpace>
void symmetrize_and_get_lower_diagonal_edge_list(
    typename in_lno_row_view_t::value_type num_rows_to_symmetrize,
    in_lno_row_view_t xadj,
    in_lno_nnz_view_t adj,
    out_lno_nnz_view_t &sym_srcs,
    out_lno_nnz_view_t &sym_dsts_
    ){

  typedef typename in_lno_row_view_t::non_const_value_type idx;


  idx nnz = adj.dimension_0();

  //idx_out_edge_array_type tmp_srcs("tmpsrc", nnz * 2);
  //idx_out_edge_array_type tmp_dsts("tmpdst",nnz * 2);

  typedef Kokkos::TeamPolicy<MyExecSpace> team_policy ;
  typedef typename team_policy::member_type team_member_t ;

  //typedef Kokkos::RangePolicy<MyExecSpace> my_exec_space;

  //TODO: Should change this to temporary memory space?
  typedef Kokkos::UnorderedMap< Kokkos::pair<idx, idx> , void , MyExecSpace> hashmap_t;

  out_lno_nnz_view_t pre_pps_("pre_pps", num_rows_to_symmetrize + 1);

  idx num_symmetric_edges = 0;
  {
    hashmap_t umap(nnz);
    umap.clear();
    umap.end_erase ();
    FillSymmetricLowerEdgesHashMap <in_lno_row_view_t, in_lno_nnz_view_t,
    hashmap_t, out_lno_nnz_view_t, team_member_t> fse(
        num_rows_to_symmetrize,
        xadj,
        adj,
        umap,
        pre_pps_
    );


    int teamSizeMax = 0;
    int vector_size = 0;
    int max_allowed_team_size = team_policy::team_size_max(fse);

    get_suggested_vector_team_size<idx, MyExecSpace>(
        max_allowed_team_size,
        vector_size,
        teamSizeMax,
        xadj.dimension_0() - 1, nnz);
    //std::cout << "max_allowed_team_size:" << max_allowed_team_size << " vs:" << vector_size << " tsm:" << teamSizeMax<< std::endl;

    Kokkos::parallel_for(
        team_policy(num_rows_to_symmetrize / teamSizeMax + 1 , teamSizeMax, vector_size),
        fse/*, num_symmetric_edges*/);
    MyExecSpace::fence();

  }

  if (num_rows_to_symmetrize > 0)
  exclusive_parallel_prefix_sum<out_lno_nnz_view_t, MyExecSpace>(
      num_rows_to_symmetrize + 1,
      pre_pps_);
  MyExecSpace::fence();

  auto d_sym_edge_size = Kokkos::subview(pre_pps_, num_rows_to_symmetrize);
  auto h_sym_edge_size = Kokkos::create_mirror_view (d_sym_edge_size);
  Kokkos::deep_copy (h_sym_edge_size, d_sym_edge_size);
  num_symmetric_edges = h_sym_edge_size();
  /*
  typename out_lno_nnz_view_t::HostMirror h_sym_edge_size = Kokkos::create_mirror_view (pre_pps_);

  Kokkos::deep_copy (h_sym_edge_size , pre_pps_);
  num_symmetric_edges = h_sym_edge_size(h_sym_edge_size.dimension_0() - 1);
  */


  sym_srcs = out_lno_nnz_view_t(Kokkos::ViewAllocateWithoutInitializing("sym_srcs"), num_symmetric_edges);
  sym_dsts_ = out_lno_nnz_view_t(Kokkos::ViewAllocateWithoutInitializing("sym_dsts_"), num_symmetric_edges);
  MyExecSpace::fence();
  {

    hashmap_t umap (nnz);
    FillSymmetricEdgeList_HashMap <in_lno_row_view_t, in_lno_nnz_view_t,
    hashmap_t, out_lno_nnz_view_t, out_lno_nnz_view_t, team_member_t>
    FSCH (num_rows_to_symmetrize, xadj, adj, umap, sym_srcs, sym_dsts_, pre_pps_);

    int teamSizeMax = 0;
    int vector_size = 0;
    int max_allowed_team_size = team_policy::team_size_max(FSCH);

    get_suggested_vector_team_size<idx, MyExecSpace>(
        max_allowed_team_size,
        vector_size,
        teamSizeMax,
        xadj.dimension_0() - 1, nnz);

    Kokkos::parallel_for(
        team_policy(num_rows_to_symmetrize / teamSizeMax + 1 , teamSizeMax, vector_size),
        FSCH);
    MyExecSpace::fence();
  }

  MyExecSpace::fence();

}


template <typename in_lno_row_view_t,
          typename in_lno_nnz_view_t,
          typename out_lno_row_view_t,
          typename out_lno_nnz_view_t,
          typename MyExecSpace>
void symmetrize_graph_symbolic_hashmap(
    typename in_lno_row_view_t::value_type num_rows_to_symmetrize,
    in_lno_row_view_t xadj,
    in_lno_nnz_view_t adj,
    out_lno_row_view_t &sym_xadj,
    out_lno_nnz_view_t &sym_adj
    ){


  typedef typename in_lno_row_view_t::non_const_value_type idx;

  idx nnz = adj.dimension_0();


  //idx_out_edge_array_type tmp_srcs("tmpsrc", nnz * 2);
  //idx_out_edge_array_type tmp_dsts("tmpdst",nnz * 2);

  typedef Kokkos::TeamPolicy<MyExecSpace> team_policy ;
  typedef typename team_policy::member_type team_member_t ;

  //typedef Kokkos::RangePolicy<MyExecSpace> my_exec_space;

  //TODO: Should change this to temporary memory space?
  typedef Kokkos::UnorderedMap< Kokkos::pair<idx, idx> , void , MyExecSpace> hashmap_t;

  out_lno_row_view_t pre_pps_("pre_pps", num_rows_to_symmetrize + 1);

  idx num_symmetric_edges = 0;
  {
    hashmap_t umap(nnz);
    umap.clear();
    umap.end_erase ();
    FillSymmetricEdgesHashMap <in_lno_row_view_t, in_lno_nnz_view_t,
    hashmap_t, out_lno_row_view_t, team_member_t> fse(
        num_rows_to_symmetrize,
        xadj,
        adj,
        umap,
        pre_pps_
    );


    int teamSizeMax = 0;
    int vector_size = 0;
    int max_allowed_team_size = team_policy::team_size_max(fse);

    get_suggested_vector_team_size<idx, MyExecSpace>(
        max_allowed_team_size,
        vector_size,
        teamSizeMax,
        xadj.dimension_0() - 1, nnz);


    Kokkos::parallel_for(
        team_policy(num_rows_to_symmetrize / teamSizeMax + 1 , teamSizeMax, vector_size),
        fse/*, num_symmetric_edges*/);
    MyExecSpace::fence();
  }


  if (num_rows_to_symmetrize > 0)
  exclusive_parallel_prefix_sum<out_lno_row_view_t, MyExecSpace>(
      num_rows_to_symmetrize + 1,
      pre_pps_);
  MyExecSpace::fence();


  //out_lno_row_view_t d_sym_edge_size = Kokkos::subview(pre_pps_, num_rows_to_symmetrize, num_rows_to_symmetrize );
  typename out_lno_row_view_t::HostMirror h_sym_edge_size = Kokkos::create_mirror_view (pre_pps_);

  Kokkos::deep_copy (h_sym_edge_size , pre_pps_);
  num_symmetric_edges = h_sym_edge_size(h_sym_edge_size.dimension_0() - 1);


  sym_adj = out_lno_nnz_view_t(Kokkos::ViewAllocateWithoutInitializing("sym_adj"), num_symmetric_edges);
  MyExecSpace::fence();
  sym_xadj = out_lno_row_view_t(Kokkos::ViewAllocateWithoutInitializing("sym_xadj"), num_rows_to_symmetrize + 1);
  Kokkos::deep_copy(sym_xadj, pre_pps_);
  {

    hashmap_t umap (nnz);
    FillSymmetricCRS_HashMap <in_lno_row_view_t, in_lno_nnz_view_t,
    hashmap_t, out_lno_row_view_t, out_lno_nnz_view_t, team_member_t>
    FSCH (num_rows_to_symmetrize, xadj, adj, umap, pre_pps_, sym_adj);

    int teamSizeMax = 0;
    int vector_size = 0;
    int max_allowed_team_size = team_policy::team_size_max(FSCH);

    get_suggested_vector_team_size<idx, MyExecSpace>(
        max_allowed_team_size,
        vector_size,
        teamSizeMax,
        xadj.dimension_0() - 1, nnz);

    Kokkos::parallel_for(
        team_policy(num_rows_to_symmetrize / teamSizeMax + 1 , teamSizeMax, vector_size),
        FSCH);
    MyExecSpace::fence();
  }

  MyExecSpace::fence();

}

template <typename idx_array_type, typename idx_edge_array_type,
          typename idx_out_array_type, typename idx_out_edge_array_type, typename MyExecSpace>
void symmetrize_graph_symbolic(
    typename idx_array_type::value_type num_rows_to_symmetrize,
    idx_array_type xadj,
    idx_edge_array_type adj,
    idx_out_array_type &sym_xadj,
    idx_out_edge_array_type &sym_adj
    ){

  typedef typename idx_array_type::value_type idx;


  idx nnz = adj.dimension_0();

  idx_out_edge_array_type tmp_srcs("tmpsrc", nnz * 2);
  idx_out_edge_array_type tmp_dsts("tmpdst",nnz * 2);

  typedef Kokkos::TeamPolicy<MyExecSpace> team_policy ;
  typedef typename team_policy::member_type team_member ;

  typedef Kokkos::RangePolicy<MyExecSpace> my_exec_space;

  FillSymmetricEdges <idx_array_type,idx_edge_array_type,idx_out_edge_array_type, team_member> fse(
      num_rows_to_symmetrize,
      xadj,
      adj,
      tmp_srcs,
      tmp_dsts
      );



  int teamSizeMax = 0;
  int vector_size = 0;
  int max_allowed_team_size = team_policy::team_size_max(fse);

  get_suggested_vector_team_size<idx, MyExecSpace>(
      max_allowed_team_size,
      vector_size,
      teamSizeMax,
      xadj.dimension_0() - 1, nnz);
  //std::cout << "max_allowed_team_size:" << max_allowed_team_size << " vs:" << vector_size << " tsm:" << teamSizeMax<< std::endl;

  Kokkos::parallel_for(
            team_policy(num_rows_to_symmetrize / teamSizeMax + 1 , teamSizeMax, vector_size),
            fse);
  MyExecSpace::fence();

#ifndef SLOWSORT
  KokkosKernelsSorting::sort_key_value_views <idx_out_edge_array_type, idx_out_edge_array_type, MyExecSpace>(tmp_srcs, tmp_dsts);
#else
  {


    typedef Kokkos::SortImpl::DefaultBinOp1D<idx_out_edge_array_type> CompType;
    Kokkos::SortImpl::min_max<typename idx_out_edge_array_type::non_const_value_type> val;
    Kokkos::parallel_reduce(tmp_srcs.dimension_0(),Kokkos::SortImpl::min_max_functor<idx_out_edge_array_type>(tmp_srcs),val);
    Kokkos::fence();
    Kokkos::BinSort<idx_out_edge_array_type, CompType> bin_sort(tmp_srcs,CompType(tmp_srcs.dimension_0()/2,val.min,val.max),true);
    bin_sort.create_permute_vector();
    bin_sort.sort(tmp_srcs);
    bin_sort.sort(tmp_dsts);
  }
#endif


  MyExecSpace::fence();

  idx_out_edge_array_type pps("PPS", nnz * 2);

  typename idx_out_edge_array_type::non_const_value_type num_symmetric_edges = 0;
  if (nnz > 0)
  Kokkos::parallel_reduce(
            my_exec_space(0, nnz * 2),
            MarkDuplicateSortedKeyValuePairs<idx_out_edge_array_type, idx_out_edge_array_type, idx_out_edge_array_type>(
                tmp_srcs, tmp_dsts, pps, nnz * 2), num_symmetric_edges);

  Kokkos::fence();
  if (nnz > 0)
  exclusive_parallel_prefix_sum<idx_out_edge_array_type, MyExecSpace>(nnz * 2, pps);

  MyExecSpace::fence();
  sym_xadj = idx_out_array_type("sym_xadj", num_rows_to_symmetrize + 1);
  sym_adj = idx_out_edge_array_type("sym_adj", num_symmetric_edges);

  MyExecSpace::fence();
  Kokkos::parallel_for(
        my_exec_space(0, nnz * 2),
        FillSymmetricCSR<idx_out_edge_array_type, idx_out_edge_array_type, idx_out_edge_array_type, idx_out_array_type, idx_out_edge_array_type>
  (tmp_srcs, tmp_dsts, pps, nnz * 2, sym_xadj, sym_adj));

  MyExecSpace::fence();
  remove_zeros_in_xadj_vector<idx_out_array_type, MyExecSpace>(num_rows_to_symmetrize + 1, sym_xadj);
  MyExecSpace::fence();
}

template <typename from_vector, typename to_vector>
struct CopyVector{
  from_vector from;
  to_vector to;

  CopyVector(from_vector &from_, to_vector to_): from(from_), to(to_){}

  KOKKOS_INLINE_FUNCTION
  void operator()(const size_t &i) const {
    to[i] = from[i];
  }
};
template <typename from_vector, typename to_vector, typename MyExecSpace>
void copy_vector(
                size_t num_elements,
                from_vector from, to_vector to){

  typedef Kokkos::RangePolicy<MyExecSpace> my_exec_space;
  Kokkos::parallel_for( my_exec_space(0,num_elements), CopyVector<from_vector, to_vector>(from, to));

}


template<typename view_type>
struct ReduceSumFunctor{

  view_type view_to_reduce;

  ReduceSumFunctor(
      view_type view_to_reduce_): view_to_reduce(view_to_reduce_){}

  void operator()(const size_t &i, typename view_type::non_const_value_type &sum_reduction) const {
    sum_reduction += view_to_reduce(i);
  }
};

template <typename view_type , typename MyExecSpace>
void view_reduce_sum(size_t num_elements, view_type view_to_reduce, typename view_type::non_const_value_type &sum_reduction){
  typedef Kokkos::RangePolicy<MyExecSpace> my_exec_space;
  Kokkos::parallel_reduce( my_exec_space(0,num_elements), ReduceSumFunctor<view_type>(view_to_reduce), sum_reduction);
}
}
}
}
#endif
