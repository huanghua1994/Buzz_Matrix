#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <mpi.h>
#include <omp.h>

#include "GTMatrix.h"
#include "utils.h"

#define ACTOR_RANK 5

/*
Run with: mpirun -np 16 ./test_Symmetrize.x
Correct output:
Initial matrix:
 10.0000     10.0000     11.0000     11.0000     11.0000     12.0000     12.0000     13.0000     13.0000     13.0000    
 14.0000     14.0000     15.0000     15.0000     15.0000     16.0000     16.0000     17.0000     17.0000     17.0000    
 14.0000     14.0000     15.0000     15.0000     15.0000     16.0000     16.0000     17.0000     17.0000     17.0000    
 14.0000     14.0000     15.0000     15.0000     15.0000     16.0000     16.0000     17.0000     17.0000     17.0000    
 18.0000     18.0000     19.0000     19.0000     19.0000     20.0000     20.0000     21.0000     21.0000     21.0000    
 18.0000     18.0000     19.0000     19.0000     19.0000     20.0000     20.0000     21.0000     21.0000     21.0000    
 22.0000     22.0000     23.0000     23.0000     23.0000     24.0000     24.0000     25.0000     25.0000     25.0000    
 22.0000     22.0000     23.0000     23.0000     23.0000     24.0000     24.0000     25.0000     25.0000     25.0000    
 22.0000     22.0000     23.0000     23.0000     23.0000     24.0000     24.0000     25.0000     25.0000     25.0000    
 22.0000     22.0000     23.0000     23.0000     23.0000     24.0000     24.0000     25.0000     25.0000     25.0000    

Symmetrized matrix:
 10.0000     12.0000     12.5000     12.5000     14.5000     15.0000     17.0000     17.5000     17.5000     17.5000    
 12.0000     14.0000     14.5000     14.5000     16.5000     17.0000     19.0000     19.5000     19.5000     19.5000    
 12.5000     14.5000     15.0000     15.0000     17.0000     17.5000     19.5000     20.0000     20.0000     20.0000    
 12.5000     14.5000     15.0000     15.0000     17.0000     17.5000     19.5000     20.0000     20.0000     20.0000    
 14.5000     16.5000     17.0000     17.0000     19.0000     19.5000     21.5000     22.0000     22.0000     22.0000    
 15.0000     17.0000     17.5000     17.5000     19.5000     20.0000     22.0000     22.5000     22.5000     22.5000    
 17.0000     19.0000     19.5000     19.5000     21.5000     22.0000     24.0000     24.5000     24.5000     24.5000    
 17.5000     19.5000     20.0000     20.0000     22.0000     22.5000     24.5000     25.0000     25.0000     25.0000    
 17.5000     19.5000     20.0000     20.0000     22.0000     22.5000     24.5000     25.0000     25.0000     25.0000    
 17.5000     19.5000     20.0000     20.0000     22.0000     22.5000     24.5000     25.0000     25.0000     25.0000
*/

int main(int argc, char **argv)
{
    int provided;
    MPI_Init_thread(&argc, &argv, MPI_THREAD_MULTIPLE, &provided);
    
    int r_displs[5] = {0, 1, 4, 6, 10};
    int c_displs[5] = {0, 2, 5, 7, 10};
    double mat[100];
    
    int my_rank;
    MPI_Comm_rank(MPI_COMM_WORLD, &my_rank);
    
    MPI_Comm comm_world;
    MPI_Comm_dup(MPI_COMM_WORLD, &comm_world);
    
    GTMatrix_t gt_mat;
    
    // 4 * 4 proc grid, matrix size 10 * 10
    GTM_createGTMatrix(
        &gt_mat, comm_world, MPI_DOUBLE, 8, my_rank, 10, 10, 
        4, 4, &r_displs[0], &c_displs[0]
    );
    
    // Set local data
    double d_fill = (double) (my_rank + 10);
    GTM_fillGTMatrix(gt_mat, &d_fill);
    
    GTM_Sync(gt_mat);
    
    if (my_rank == ACTOR_RANK)
    {
        GTM_getBlock(gt_mat, 0, 10, 0, 10, &mat[0], 10, 1);
        print_double_mat(&mat[0], 10, 10, 10, "Initial matrix");
    }
    
    GTM_Sync(gt_mat);
    
    // Symmetrizing
    GTM_symmetrizeGTMatrix(gt_mat);
    if (my_rank == ACTOR_RANK)
    {
        GTM_getBlock(gt_mat, 0, 10, 0, 10, &mat[0], 10, 1);
        print_double_mat(&mat[0], 10, 10, 10, "Symmetrized matrix");
    }
    
    GTM_Sync(gt_mat);
    
    GTM_destroyGTMatrix(gt_mat);
    
    MPI_Finalize();
}