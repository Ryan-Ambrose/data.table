#include "data.table.h"

/*
 * sort-merge join
 *
 * join on a single integer column
 * sort LHS and RHS
 * split LHS into equal batches, RHS into corresponding batches by matching upper-lower bounds of LHS batches
 * parallel sort-merge join
 *
 * for a maximum speed collecting of following statistics can be disabled by undefine SMERGE_STATS
 * y_len1/y_lens1/allLen1:       signals if there where multiple matches in RHS of join, ['s _x_ table
 * x_len1/x_lens1/lhsLen1:       signals if multiple matches in LHS of join, ['s _i_ table
 * xy_len1/xy_lens1/xlLen1:      signals if "many to many" matches between LHS and RHS
 * cnt/nmatch/n_match/n_matchr:  count of matches, taking multiple matches into account, uint64_t
 *
 * when hardcoding or changing default number of batches it is advised to undefine SMERGE_BATCHING_BALANCED
 */
#define SMERGE_STATS
#define SMERGE_BATCHING_BALANCED

// this only refers to RHS side: y
enum emult{ALL, FIRST, LAST, ERR};

// workhorse join that runs in parallel on batches
static void smerge(const int bx_off, const int bnx,
                   const int by_off, const int bny,
                   const int *restrict x, const int *restrict x_starts, const int *restrict x_lens, const bool unq_x,
                   const int *restrict y, const int *restrict y_starts, const int *restrict y_lens, const bool unq_y,
                   int *restrict starts, int *restrict lens,
                   uint64_t *nmatch, bool *xlens1, bool *ylens1, bool *xylens1,
                   const enum emult mult) {
  uint64_t cnt = 0;
  bool xlen1 = true, ylen1 = true, xylen1 = true;
  if (unq_x && unq_y) {
    int i = bx_off, j = by_off;
    const int ni = bx_off+bnx, nj = by_off+bny;
    while (i<ni && j<nj) {
      const int x_i = x[i], y_j = y[j];
      if (x_i == y_j) {
        starts[i] = j+1;
        //lens[i] = 1; // already filled with 1's we will need this line if we change default alloc
        i++;
#ifdef SMERGE_STATS
        cnt++;
#endif
      } else if (x_i < y_j) {
        i++;
      } else if (x_i > y_j) {
        j++;
      }
    }
  } else if (unq_x) {
    int i = bx_off, js = by_off;
    const int ni = bx_off+bnx, njs = by_off+bny;
    if (mult == ALL || mult == ERR) { // mult==err is raised based on ylens1 flag outside of parallel region
      while (i<ni && js<njs) {
        const int j = y_starts[js]-1;
        const int x_i = x[i], y_j = y[j];
        if (x_i == y_j) {
          starts[i] = j+1;
          const int yl1 = y_lens[js];
          lens[i] = yl1;
          i++;
#ifdef SMERGE_STATS
          if (ylen1 && yl1>1)
            ylen1 = false;
          cnt += (uint64_t)yl1;
#endif
        } else if (x_i < y_j) {
          i++;
        } else if (x_i > y_j) {
          js++;
        }
      }
    } else if (mult == FIRST) {
      while (i<ni && js<njs) {
        const int j = y_starts[js]-1;
        const int x_i = x[i], y_j = y[j];
        if (x_i == y_j) {
          starts[i] = j+1;
          //lens[i] = 1;
          i++;
#ifdef SMERGE_STATS
          cnt++;
#endif
        } else if (x_i < y_j) {
          i++;
        } else if (x_i > y_j) {
          js++;
        }
      }
    } else if (mult == LAST) {
      while (i<ni && js<njs) {
        const int j = y_starts[js]-1;
        const int x_i = x[i], y_j = y[j];
        if (x_i == y_j) {
          starts[i] = j+y_lens[js];
          //lens[i] = 1;
          i++;
#ifdef SMERGE_STATS
          cnt++;
#endif
        } else if (x_i < y_j) {
          i++;
        } else if (x_i > y_j) {
          js++;
        }
      }
    }
  } else if (unq_y) {
    int is = bx_off, j = by_off;
    const int nis = bx_off+bnx, nj = by_off+bny;
    while (is<nis && j<nj) {
      const int i = x_starts[is]-1;
      const int x_i = x[i], y_j = y[j];
      if (x_i == y_j) {
        const int j1 = j+1;
        const int xl1 = x_lens[is];
        for (int ii=0; ii<xl1; ++ii) {
          starts[i+ii] = j1;
          //lens[i+ii] = 1;
        }
        is++;
#ifdef SMERGE_STATS
        if (xlen1 && xl1>1)
          xlen1 = false;
        cnt += (uint64_t)xl1;
#endif
      } else if (x_i < y_j) {
        is++;
      } else if (x_i > y_j) {
        j++;
      }
    }
  } else {
    int is = bx_off, js = by_off;
    const int nis = bx_off+bnx, njs = by_off+bny;
    if (mult==ALL || mult==ERR) {
      while (is<nis && js<njs) {
        const int i = x_starts[is]-1, j = y_starts[js]-1;
        const int x_i = x[i], y_j = y[j];
        if (x_i == y_j) {
          const int j1 = j+1, yl1 = y_lens[js];
          const int xl1 = x_lens[is];
          for (int ii=0; ii<xl1; ++ii) {
            starts[i+ii] = j1;
            lens[i+ii] = yl1;
          }
          is++;
  #ifdef SMERGE_STATS
          if (xlen1 && xl1>1)
            xlen1 = false;
          if (ylen1 && yl1>1)
            ylen1 = false;
          if (xylen1 && xl1>1 && yl1>1)
            xylen1 = false;
          cnt += (uint64_t)xl1 * (uint64_t)yl1;
  #endif
        } else if (x_i < y_j) {
          is++;
        } else if (x_i > y_j) {
          js++;
        }
      }
    } else if (mult==FIRST) {
      while (is<nis && js<njs) {
        const int i = x_starts[is]-1, j = y_starts[js]-1;
        const int x_i = x[i], y_j = y[j];
        if (x_i == y_j) {
          const int j1 = j+1;
          const int xl1 = x_lens[is];
          for (int ii=0; ii<xl1; ++ii) {
            starts[i+ii] = j1;
            //lens[i+ii] = 1;
          }
          is++;
#ifdef SMERGE_STATS
          if (xlen1 && xl1>1)
            xlen1 = false;
          cnt += (uint64_t)xl1;
#endif
        } else if (x_i < y_j) {
          is++;
        } else if (x_i > y_j) {
          js++;
        }
      }
    } else if (mult==LAST) {
      while (is<nis && js<njs) {
        const int i = x_starts[is]-1, j = y_starts[js]-1;
        const int x_i = x[i], y_j = y[j];
        if (x_i == y_j) {
          const int j1 = j+y_lens[js];
          const int xl1 = x_lens[is];
          for (int ii=0; ii<xl1; ++ii) {
            starts[i+ii] = j1;
            //lens[i+ii] = 1;
          }
          is++;
#ifdef SMERGE_STATS
          if (xlen1 && xl1>1)
            xlen1 = false;
          cnt += (uint64_t)xl1;
#endif
        } else if (x_i < y_j) {
          is++;
        } else if (x_i > y_j) {
          js++;
        }
      }
    }
  }
  xlens1[0] = xlen1; ylens1[0] = ylen1; xylens1[0] = xylen1; nmatch[0] = cnt;
  return;
}

/*
 * 'rolling nearest' binary search
 * used to find 'y' lower and upper, 0-based, bounds for each batch
 * side -1: lower bound; side 1: upper bound
 */
static int rollbs(const int *restrict x, const int *restrict ix, const int nix, const int val, const int side) {
  int verbose = 0; // devel debug only
  if (verbose>0)
    Rprintf("rollbs: side=%d=%s; val=%d\n", side, side<0?"min":"max", val); // # nocov
  if (x[ix[0]-1] == val) { // common early stopping
    if (verbose>0)
      Rprintf("rollbs: min elements %d match: 0\n", val); // # nocov
    return 0;
  }
  if (x[ix[nix-1]-1] == val) {
    if (verbose>0)
      Rprintf("rollbs: max elements %d match: nix-1=%d\n", val, nix-1); // # nocov
    return nix-1;
  }
  if (side < 0) { // lower bound early stopping
    if (x[ix[nix-1]-1] < val) {
      if (verbose>0)
        Rprintf("rollbs: max element %d is still smaller than %d: -1\n", x[ix[nix-1]-1], val); // # nocov
      return -1;
    }
    if (x[ix[0]-1] > val) {
      if (verbose>0)
        Rprintf("rollbs: min element %d is bigger than %d: 0\n", x[ix[0]-1], val); // # nocov
      return 0;
    }
  } else if (side > 0) { // upper bound early stopping
    if (x[ix[0]-1] > val) {
      if (verbose>0)
        Rprintf("rollbs: min element %d is still bigger than %d: -1\n", x[ix[0]-1], val); // # nocov
      return -1;
    }
    if (x[ix[nix-1]-1] < val) {
      if (verbose>0)
        Rprintf("rollbs: max element %d is smaller than %d: nix-1=%d\n", x[ix[nix-1]-1], val, nix-1); // # nocov
      return nix-1;
    }
  }
  int lower=0, upper=nix, i=0;
  while (lower<=upper) {
    i = lower + (upper-lower)/2;
    int thisx = x[ix[i]-1];
    //Rprintf("rollbs: x[ix[%d]-1]=%d ?? %d\n", i, thisx, val); // if (verbose) here would slow down while loop so need to be commented out
    if (thisx==val)
      return(i);
    else if (thisx < val)
      lower = i+1;
    else if (thisx > val)
      upper = i-1;
  }
  if (verbose>0)
    Rprintf("rollbs: nomatch: i=%d; this=%d; lower=%d, upper=%d; side=%d: %d\n", i, x[ix[i]-1], lower, upper, side, side<0?lower:upper); // # nocov
  if (side < 0) // anyone to stress test this logic?
    return lower;
  else
    return upper;
}

// cuts x_starts into equal batches and binary search corresponding y_starts ranges
static void batching(const int nBatch,
                     const int *restrict x, const int nx, const int *restrict x_starts, const int nx_starts,
                     const int *restrict y, const int ny, const int *restrict y_starts, const int ny_starts,
                     int *restrict Bx_off, int *restrict Bnx, int *restrict By_off, int *restrict Bny,
                     const int verbose) {
#ifdef SMERGE_BATCHING_BALANCED
  //if (nBatch > nx_starts || (nx_starts/nBatch==1 && nx_starts%nBatch>0)) error("internal error: batching %d input into %d batches, number of batches should have been reduced be now", nx_starts, nBatch); // # nocov
  size_t batchSize = (nx_starts-1)/nBatch + 1; // this is fragile for arbitrary nBatch
  bool balanced = true; // this is only for verbose message
#else
  size_t batchSize = nx_starts / nBatch; // last batch size can be anything between 1 and 2*batchSize-1
  bool balanced = false;
#endif
  size_t lastBatchSize = nx_starts - (nBatch-1)*batchSize;
  if (verbose>0)
    Rprintf("batching: input %d into %s %d batches (batchSize=%d, lastBatchSize=%d) of sorted x y: x[1]<=y[1] && x[nx]>=y[ny]:\n", nx_starts, balanced?"balanced":"unbalanced", nBatch, batchSize, lastBatchSize);
  if (lastBatchSize==0 || ((nBatch-1) * batchSize + lastBatchSize != nx_starts))
    error("internal error: batching %d input is attempting to use invalid batches: balanced=%d, nBatch=%d, batchSize=%d, lastBatchSize=%d", nx_starts, balanced?"balanced":"unbalanced", nBatch, batchSize, lastBatchSize); // # nocov
  for (int b=0; b<nBatch; ++b) {
    Bx_off[b] = b<nBatch-1 ? b*batchSize : nx_starts-lastBatchSize;
    Bnx[b] = b<nBatch-1 ? batchSize : lastBatchSize;
    int x_i_min = (x_starts + Bx_off[b])[0];
    int x_i_max = (x_starts + Bx_off[b])[Bnx[b]-1];
    int y_i_min = rollbs(y, y_starts, ny_starts, x[x_i_min-1], -1);
    int y_i_max = rollbs(y, y_starts, ny_starts, x[x_i_max-1], 1);
    const bool y_match = y_i_min >= 0 && y_i_max >= 0;
    By_off[b] = y_match ? y_i_min : 0;
    Bny[b] = y_match ? y_i_max - y_i_min + 1 : 0;
  }
  if (verbose>0) { // print batches, 1-indexed! x y sorted! for debugging and verbose
    for (int b=0; b<nBatch; ++b) {
      Rprintf("#### batch[%d]: unq n: x=%d, y=%d\n", b+1, Bnx[b], Bny[b]);
      if (Bny[b] > 0) {
        int x_i_min = (x_starts + Bx_off[b])[0], x_i_max = (x_starts + Bx_off[b])[Bnx[b]-1];
        int y_i_min = (y_starts + By_off[b])[0], y_i_max = (y_starts + By_off[b])[Bny[b]-1];
        Rprintf("## lower: x[%d]: %d <= %d :y[%d]\n", x_i_min, x[x_i_min-1], y[y_i_min-1], y_i_min);
        Rprintf("## upper: x[%d]: %d >= %d :y[%d]\n", x_i_max, x[x_i_max-1], y[y_i_max-1], y_i_max);
      }
    }
  }
  return;
}

// helper for verbose messages to count how many threads were used, due to schedule dynamic not all may be used
static int unqNth(const int *x, const int nx) { // x have 0:(nx-1) values
  int ans = 0;
  uint8_t *seen = (uint8_t *)R_alloc(nx, sizeof(uint8_t));
  memset(seen, 0, nx*sizeof(uint8_t));
  for (int i=0; i<nx; ++i) {
    if (!seen[x[i]]++) ans++;
  }
  return ans;
}

// helper for verbose messages to print what was actually computed
static const char *verboseDone(const bool x, const bool y, const char *not_xy, const char *not_x, const char *not_y, const char *xy) {
  if (!x && !y)
    return not_xy;
  else if (!x && y)
    return not_x;
  else if (x && !y)
    return not_y;
  else
    return xy;
}

// count of duplicated entries
void grpLens(const int *restrict starts, const int n_starts, const int n, int *restrict lens) { // #4395
  #pragma omp parallel for schedule(static) num_threads(getDTthreads())
  for (int i=0; i<n_starts-1; ++i)
    lens[i] = starts[i+1]-starts[i];
  lens[n_starts-1] = n - starts[n_starts-1]+1;
  return;
}

// pure C smerge: takes already sorted input, compute grpLens, prepare batches, smerge in parallel
void smergeC(const int *restrict x, const int nx, const int *restrict x_starts, const int nx_starts,
             const int *restrict y, const int ny, const int *restrict y_starts, const int ny_starts,
             int *restrict starts, int *restrict lens,
             uint64_t *n_match, bool *x_lens1, bool *y_lens1, bool *xy_lens1,
             const enum emult mult, const int verbose) {

  double t = 0;
  if (verbose>0)
    t = omp_get_wtime();
  int *restrict x_lens = 0, *restrict y_lens = 0;
  const bool unq_x = nx_starts==nx, unq_y = ny_starts==ny;
  if (!unq_x) {
    x_lens = (int *)R_alloc(nx_starts, sizeof(int)); // remove after #4395
    grpLens(x_starts, nx_starts, nx, x_lens);
  }
  if (!(unq_y || mult==FIRST)) {
    y_lens = (int *)R_alloc(ny_starts, sizeof(int));
    grpLens(y_starts, ny_starts, ny, y_lens);
  }
  if (verbose>0)
    Rprintf("smergeC: grpLens %s took %.3fs\n", verboseDone(!unq_x, !(unq_y || mult==FIRST), "(x already unq, y unq or mult='first')", "(y)", "(x)", "(x, y)"), omp_get_wtime() - t);

  if (verbose>0)
    t = omp_get_wtime();
  int nBatch = 0;
  const int nth = getDTthreads();
  if (nth == 1 || nx_starts < 1024) {
#ifdef SMERGE_BATCHING_BALANCED
    nBatch = 1; // when using balanced lastBatchSize is never bigger than batchSize, any hardcoding here is likely to raise internal error in batching or segfault, testing possible with: for (i in 1:10) cc("smerge.Rraw")
#else
    nBatch = 1; // so if hardcoding needed, do it here, and undefine SMERGE_BATCHING_BALANCED at the top, for unbalanced batching lastBatchSize is anything between 1 and 2*batchSize-1
#endif
  } else if (nx_starts < nth * 2) {
    nBatch = nx_starts; // stress test single row batches, will be usually escaped by branch above
  } else {
    nBatch = nth * 2;
  }
  int *restrict Bx_off = (int *)R_alloc(nBatch, sizeof(int)), *restrict Bnx = (int *)R_alloc(nBatch, sizeof(int));
  int *restrict By_off = (int *)R_alloc(nBatch, sizeof(int)), *restrict Bny = (int *)R_alloc(nBatch, sizeof(int));
  batching(nBatch, x, nx, x_starts, nx_starts, y, ny, y_starts, ny_starts, Bx_off, Bnx, By_off, Bny, verbose-1);
  int *restrict th = (int *)R_alloc(nBatch, sizeof(int)); // report threads used
  if (verbose>0)
    Rprintf("smergeC: preparing %d batches took %.3fs\n", nBatch, omp_get_wtime() - t);

  if (verbose>0)
    t = omp_get_wtime();
  uint64_t nmatch = 0;
  bool xlens1 = true, ylens1 = true, xylens1 = true;
  #pragma omp parallel for schedule(dynamic) reduction(&&:xlens1,ylens1,xylens1) reduction(+:nmatch) num_threads(nth)
  for (int b=0; b<nBatch; ++b) {
    uint64_t bnmatch = 0; // local reduction variables
    bool bxlens1 = true, bylens1 = true, bxylens1 = true;
    smerge(
      Bx_off[b], Bnx[b],                      // batch input x
      By_off[b], Bny[b],                      // batch input y
      x, x_starts, x_lens, unq_x,             // common input x
      y, y_starts, y_lens, unq_y,             // common input y
      starts, lens,                           // common output
      &bnmatch, &bxlens1, &bylens1, &bxylens1,// reduction output
      mult
    );
    nmatch += bnmatch; xlens1 = bxlens1 && xlens1; ylens1 = bylens1 && ylens1; xylens1 = bxylens1 && xylens1;
    th[b] = omp_get_thread_num(); // only to report thread utilization in verbose message
  }
  x_lens1[0] = xlens1; y_lens1[0] = ylens1; xy_lens1[0] = xylens1; n_match[0] = nmatch;
  if (verbose>0)
    Rprintf("smergeC: %d calls to smerge using %d/%d threads took %.3fs\n", nBatch, unqNth(th, nBatch), nth, omp_get_wtime() - t); // all threads may not always be used bc schedule(dynamic)
  if (mult==ERR && !ylens1)
    error("mult='error' and multiple matches during merge");

  return;
}

void sortInt(const int *restrict x, const int nx, const int *restrict idx, int *restrict ans) {
  #pragma omp parallel for schedule(static) num_threads(getDTthreads())
  for (int i=0; i<nx; ++i)
    ans[i] = x[idx[i]-1];
  return;
}
void copyInt(const int *restrict x, const int nx, int *restrict ans) {
  #pragma omp parallel for schedule(static) num_threads(getDTthreads())
  for (int i=0; i<nx; ++i)
    ans[i] = x[i];
  return;
}

// wrap results into list, bmerge=true produces bmerge replacement output, note that bmerge i,x is smerge x,y (as of now)
SEXP outSmergeR(const int n, const int *restrict starts, const int *restrict lens, const bool x_ord,
                SEXP out_starts, SEXP out_lens, // used only when x was sorted, saves one allocation
                SEXP x_idx, SEXP y_idx,
                uint64_t n_match, bool x_lens1, bool y_lens1, bool xy_lens1,
                const bool multLen1, const bool bmerge) {
  const int out_len = bmerge ? 6 : 10;
  SEXP ans = PROTECT(allocVector(VECSXP, out_len)), ansnames;
  setAttrib(ans, R_NamesSymbol, ansnames=allocVector(STRSXP, out_len));
  SET_STRING_ELT(ansnames, 0, char_starts);
  SET_STRING_ELT(ansnames, 1, char_lens);
  if (bmerge) { // for bmerge we still need allocate, but not unsort, for !bmerge no alloc no unsort
    if (x_ord) {
      SET_VECTOR_ELT(ans, 0, out_starts);
      SET_VECTOR_ELT(ans, 1, out_lens);
    } else {
      SET_VECTOR_ELT(ans, 0, allocVector(INTSXP, n));
      SET_VECTOR_ELT(ans, 1, allocVector(INTSXP, n));
      SEXP xoo = PROTECT(forder(x_idx, R_NilValue, /*retGrp=*/ScalarLogical(false), ScalarLogical(true), ScalarInteger(1), ScalarLogical(false))); // verbose=verbose-2L after #4533
      sortInt(starts, n, INTEGER(xoo), INTEGER(VECTOR_ELT(ans, 0)));
      if (!y_lens1) // no need to unsort 1-only vector
        sortInt(lens, n, INTEGER(xoo), INTEGER(VECTOR_ELT(ans, 1)));
      else
        copyInt(lens, n, INTEGER(VECTOR_ELT(ans, 1)));
      UNPROTECT(1);
    }
  } else {
    const bool skipLens = !multLen1 && y_lens1;
    if (skipLens) { // compact lens if ylens1, mult=first|last already has compact lens
      out_lens = PROTECT(allocVector(INTSXP, 0));
    }
    if (x_ord) {
      if (y_lens1 && LENGTH(out_lens))
        error("internal error: lens should be already compact 0 length integer"); // # nocov
      SET_VECTOR_ELT(ans, 0, out_starts);
      SET_VECTOR_ELT(ans, 1, out_lens);
    } else {
      SET_VECTOR_ELT(ans, 0, allocVector(INTSXP, n));
      SET_VECTOR_ELT(ans, 1, allocVector(INTSXP, y_lens1 ? 0 : n));
      SEXP xoo = PROTECT(forder(x_idx, R_NilValue, /*retGrp=*/ScalarLogical(false), ScalarLogical(true), ScalarInteger(1), ScalarLogical(false))); // verbose=verbose-2L after #4533
      sortInt(starts, n, INTEGER(xoo), INTEGER(VECTOR_ELT(ans, 0)));
      if (!y_lens1) // no need to unsort 1-only vector, it is now compact 0 length anyway
        sortInt(lens, n, INTEGER(xoo), INTEGER(VECTOR_ELT(ans, 1)));
      UNPROTECT(1);
    }
    if (skipLens)
      UNPROTECT(1);
  }
  SET_STRING_ELT(ansnames, 2, char_indices); SET_VECTOR_ELT(ans, 2, allocVector(INTSXP, 0)); // const for equi join
  SET_STRING_ELT(ansnames, 3, char_allLen1); SET_VECTOR_ELT(ans, 3, ScalarLogical(y_lens1));
  SET_STRING_ELT(ansnames, 4, char_allGrp1); SET_VECTOR_ELT(ans, 4, ScalarLogical(true));    // const for equi join
  if (bmerge) {
    y_idx = shallow_duplicate(y_idx); // possibly improve after #4467
    setAttrib(y_idx, sym_starts, R_NilValue);
    setAttrib(y_idx, sym_maxgrpn, R_NilValue);
    //setAttrib(y_idx, sym_anyna, R_NilValue); // enable after #4386
    //setAttrib(y_idx, sym_anyinfnan, R_NilValue);
    //setAttrib(y_idx, sym_anynotascii, R_NilValue);
    //setAttrib(y_idx, sym_anynotutf8, R_NilValue);
  }
  SET_STRING_ELT(ansnames, 5, char_xo); SET_VECTOR_ELT(ans, 5, y_idx);
  if (!bmerge) {
    SET_STRING_ELT(ansnames, 6, char_io);      SET_VECTOR_ELT(ans, 6, x_idx);
    SET_STRING_ELT(ansnames, 7, char_lhsLen1); SET_VECTOR_ELT(ans, 7, ScalarLogical(x_lens1));
    SET_STRING_ELT(ansnames, 8, char_xyLen1);  SET_VECTOR_ELT(ans, 8, ScalarLogical(xy_lens1));
    SEXP n_matchr = PROTECT(allocVector(REALSXP, 1));
    if (n_match > (uint64_t)DBL_MAX) { // 1e9 x 1e9 cartesian join results 1e18 still less than DBL_MAX, should we check against DBL_MAX or something lesser? we cast uinst64_t to double here
      REAL(n_matchr)[0] = NA_REAL;
      warning("count of matches exceeds DBL_MAX, returning NA in 'nMatch' field");
    } else {
      REAL(n_matchr)[0] = (double)n_match;
    }
    SET_STRING_ELT(ansnames, 9, char_nMatch);  SET_VECTOR_ELT(ans, 9, n_matchr);
    UNPROTECT(1);
  }
  UNPROTECT(1);
  return ans;
}

const enum emult matchMultArg(SEXP multArg) {
  enum emult mult;
  if (!strcmp(CHAR(STRING_ELT(multArg, 0)), "all"))
    mult = ALL;
  else if (!strcmp(CHAR(STRING_ELT(multArg, 0)), "first"))
    mult = FIRST;
  else if (!strcmp(CHAR(STRING_ELT(multArg, 0)), "last"))
    mult = LAST;
  else if (!strcmp(CHAR(STRING_ELT(multArg, 0)), "error"))
    mult = ERR;
  else
    error(_("Internal error: invalid value for 'mult'. please report to data.table issue tracker")); // # nocov
  return mult;
}

// main interface from R
SEXP smergeR(SEXP x, SEXP y, SEXP x_idx, SEXP y_idx, SEXP multArg, SEXP out_bmerge) {

  const int verbose = GetVerbose()*3; // remove *3 after #4491
  double t_total = 0, t = 0;
  if (verbose>0)
    t_total = omp_get_wtime();
  if (!isInteger(x) || !isInteger(y))
    error("'x' and 'y' must be integer");
  if (!isString(multArg))
    error("'mult' must be a string");
  const enum emult mult = matchMultArg(multArg);
  const bool multLen1 = mult==FIRST || mult==LAST;
  if (!IS_TRUE_OR_FALSE(out_bmerge))
    error("'out.bmerge' must be TRUE or FALSE");
  const bool ans_bmerge = (bool)LOGICAL(out_bmerge)[0];
  int protecti = 0, nx = LENGTH(x), ny = LENGTH(y);

  if (verbose>0)
    t = omp_get_wtime();
  const bool do_x_idx = isNull(x_idx), do_y_idx = isNull(y_idx);
  if (do_x_idx) {
    x_idx = PROTECT(forder(x, R_NilValue, ScalarLogical(true), ScalarLogical(true), ScalarInteger(1), ScalarLogical(false))); protecti++; // verbose=verbose-2L after #4533
  }
  if (do_y_idx) {
    y_idx = PROTECT(forder(y, R_NilValue, ScalarLogical(true), ScalarLogical(true), ScalarInteger(1), ScalarLogical(false))); protecti++; // verbose=verbose-2L after #4533
  }
  if (!isInteger(x_idx) || !isInteger(y_idx))
    error("'x.idx' and 'y.idx' must be integer");
  SEXP x_starts = getAttrib(x_idx, sym_starts); SEXP y_starts = getAttrib(y_idx, sym_starts);
  if (isNull(x_starts) || isNull(y_starts))
    error("Indices provided to smerge must carry 'starts' attribute");
  if (verbose>0)
    Rprintf("smergeR: index %s took %.3fs\n", verboseDone(do_x_idx, do_y_idx, "(already indexed)", "(y)", "(x)", "(x, y)"), omp_get_wtime() - t);

  if (verbose>0)
    t = omp_get_wtime();
  const bool x_ord = !LENGTH(x_idx), y_ord = !LENGTH(y_idx);
  int *xp, *yp;
  if (!x_ord) {
    xp = (int *)R_alloc(nx, sizeof(int));
    sortInt(INTEGER(x), nx, INTEGER(x_idx), xp);
  } else {
    xp = INTEGER(x);
  }
  if (!y_ord) {
    yp = (int *)R_alloc(ny, sizeof(int));
    sortInt(INTEGER(y), ny, INTEGER(y_idx), yp);
  } else {
    yp = INTEGER(y);
  }
  if (verbose>0)
    Rprintf("smergeR: sort %s took %.3fs\n", verboseDone(!x_ord, !y_ord, "(already sorted)", "(y)", "(x)", "(x, y)"), omp_get_wtime() - t);

  if (verbose>0)
    t = omp_get_wtime();
  SEXP out_starts = R_NilValue, out_lens = R_NilValue;
  int *restrict starts=0, *restrict lens=0;
  const int lens_len = (!multLen1 || ans_bmerge) ? nx : 0; // for mult=first|last we dont need to allocate lens
  if (x_ord) { // we dont need to reorder results so can save one allocation
    out_starts = PROTECT(allocVector(INTSXP, nx)); protecti++;
    out_lens = PROTECT(allocVector(INTSXP, lens_len)); protecti++;
    starts = INTEGER(out_starts);
    lens = INTEGER(out_lens);
  } else {
    starts = (int *)R_alloc(nx, sizeof(int));
    lens = (int *)R_alloc(lens_len, sizeof(int));
  }
  // this fills default values, bmerge's defaults are tricky (dictated by how they are consumed): nomatch=0 makes starts=0 not NA, lens=0 is fine there; nomatch=NA makes lens=1 not NA, starts=NA is fine there
  // AFAIU it make sense to take out nomatch argument from merge
  if (multLen1 && !ans_bmerge) {
    #pragma omp parallel for schedule(static) num_threads(getDTthreads())
    for (int i=0; i<nx; ++i)
      starts[i] = NA_INTEGER;
  } else {
    #pragma omp parallel for schedule(static) num_threads(getDTthreads())
    for (int i=0; i<nx; ++i) {
      starts[i] = NA_INTEGER;
      lens[i] = 1;
    }
  }
  if (verbose>0)
    Rprintf("smergeR: alloc of size %d took %.3fs\n", nx, omp_get_wtime() - t);

  if (verbose>0)
    t = omp_get_wtime();
  uint64_t n_match = 0;
  bool x_lens1 = true, y_lens1 = true, xy_lens1 = true;
  smergeC(
    xp, nx, INTEGER(x_starts), LENGTH(x_starts),
    yp, ny, INTEGER(y_starts), LENGTH(y_starts),
    starts, lens,
    &n_match, &x_lens1, &y_lens1, &xy_lens1,
    mult, verbose-1
  );
  if (verbose>0)
    Rprintf("smergeR: smergeC of %d x %d = %"PRIu64"; took %.3fs\n", nx, ny, n_match, omp_get_wtime() - t);

  if (verbose>0)
    t = omp_get_wtime();
  SEXP ans = outSmergeR(nx, starts, lens, x_ord, out_starts, out_lens, x_idx, y_idx, n_match, x_lens1, y_lens1, xy_lens1, multLen1, ans_bmerge);
  if (verbose>0)
    Rprintf("smergeR: outSmerge %s took %.3fs\n", x_ord ? "(was sorted)" : "(alloc and unsort)", omp_get_wtime() - t);
  if (verbose>0)
    Rprintf("smergeR: all took %.3fs\n", omp_get_wtime() - t_total);

  UNPROTECT(protecti);
  return ans;
}
