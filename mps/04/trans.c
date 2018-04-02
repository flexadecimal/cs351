/* 
 * trans.c - Matrix transpose B = A^T
 *
 * Each transpose function must have a prototype of the form:
 * void trans(int M, int N, int A[N][M], int B[M][N]);
 *
 * A transpose function is evaluated by counting the number of misses
 * on a 1KB direct mapped cache with a block size of 32 bytes.
 */ 
#include <stdio.h>
#include "cachelab.h"

int is_transpose(int M, int N, int A[N][M], int B[M][N]);

/* 
 * transpose_submit - This is the solution transpose function that you
 *     will be graded on for Part B of the assignment. Do not change
 *     the description string "Transpose submission", as the driver
 *     searches for that string to identify the transpose function to
 *     be graded. 
 */
char transpose_submit_desc[] = "Transpose submission";
void transpose_submit(int M, int N, int A[N][M], int B[M][N])
{
  int bsize;
  if (N == 32){
    bsize = 8;
  }
  else if (N == 64){
    bsize = 4;
  }
  else {
    bsize = 17;
  }

  int i, j, k, l, temp, idx, diag = 0;
  /* for square matrices, try to minimize diag accesses*/
  if (N == M){
    for (i = 0; i < N; i += bsize){
      for (j = 0; j < N; j += bsize){
	for (k = i; k < i+bsize && k < M; k++){
	  for (l = j; l < j+bsize && l < N; l++){
	    if (k != l){
	      B[k][l] = A[l][k];
	    }
	    else {
	      temp = A[l][k];
	      idx = k;
	      diag = 1;
	    }
	  }
	  if (diag == 1){
	    B[idx][idx] = temp;
	    diag = 0;
	  }
	}
      }
    }
  }
  else {
  for (i = 0; i < M; i += bsize){
    for (j = 0; j < N; j += bsize){
      /* tranpose block */
      for (k = i; k < i+bsize && k < M; k++){
	for (l = j; l < j+bsize && l < N; l++){
	  B[k][l] = A[l][k];
	  }
	}
      }
  }
  }
}

/* 
 * You can define additional transpose functions below. We've defined
 * a simple one below to help you get started. 
 */ 

/* 
 * trans - A simple baseline transpose function, not optimized for the cache.
 */
char trans_desc[] = "Simple row-wise scan transpose";
void trans(int M, int N, int A[N][M], int B[M][N])
{
    int i, j, tmp;

    for (i = 0; i < N; i++) {
        for (j = 0; j < M; j++) {
            tmp = A[i][j];
            B[j][i] = tmp;
        }
    }    

}

/*
 * registerFunctions - This function registers your transpose
 *     functions with the driver.  At runtime, the driver will
 *     evaluate each of the registered functions and summarize their
 *     performance. This is a handy way to experiment with different
 *     transpose strategies.
 */
void registerFunctions()
{
    /* Register your solution function */
    registerTransFunction(transpose_submit, transpose_submit_desc); 

    /* Register any additional transpose functions */
    registerTransFunction(trans, trans_desc); 

}

/* 
 * is_transpose - This helper function checks if B is the transpose of
 *     A. You can check the correctness of your transpose by calling
 *     it before returning from the transpose function.
 */
int is_transpose(int M, int N, int A[N][M], int B[M][N])
{
    int i, j;

    for (i = 0; i < N; i++) {
        for (j = 0; j < M; ++j) {
            if (A[i][j] != B[j][i]) {
                return 0;
            }
        }
    }
    return 1;
}

