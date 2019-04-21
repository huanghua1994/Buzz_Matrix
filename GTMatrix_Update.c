#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <assert.h>
#include <mpi.h>

#include "GTMatrix.h"
#include "utils.h"

// Note: For accumulation, only element-wise atomicity is needed, use MPI_LOCK_SHARED. 
//       For replacement, user should guarantee the write sequence and handle conflict,
//       still use MPI_LOCK_SHARED. 

// Update (put or accumulate) a block to a process using MPI_Accumulate
// The update operation is not complete when this function returns
// [in] gt_mat     : GTMatrix handle
// [in] dst_rank   : Target process
// [in] op         : MPI operation, only support MPI_SUM (accumulate) and MPI_REPLACE (MPI_Put)
// [in] row_start  : 1st row of the source block
// [in] row_num    : Number of rows the source block has
// [in] col_start  : 1st column of the source block
// [in] col_num    : Number of columns the source block has
// [in] *src_buf   : Source buffer
// [in] src_buf_ld : Leading dimension of the source buffer
void GTM_updateBlockToProcess(
    GTMatrix_t gt_mat, int dst_rank, MPI_Op op, 
    int row_start, int row_num,
    int col_start, int col_num,
    void *src_buf, int src_buf_ld
)
{
    int row_end       = row_start + row_num;
    int col_end       = col_start + col_num;
    int dst_rowblk    = dst_rank / gt_mat->c_blocks;
    int dst_colblk    = dst_rank % gt_mat->c_blocks;
    int dst_blk_ld    = gt_mat->ld_local; // gt_mat->ld_blks[dst_rank];
    int dst_row_start = gt_mat->r_displs[dst_rowblk];
    int dst_col_start = gt_mat->c_displs[dst_colblk];
    int dst_row_end   = gt_mat->r_displs[dst_rowblk + 1];
    int dst_col_end   = gt_mat->c_displs[dst_colblk + 1];
    
    // Sanity check
    if ((row_start < dst_row_start) ||
        (col_start < dst_col_start) ||
        (row_end   > dst_row_end)   ||
        (col_end   > dst_col_end)   ||
        (row_num   * col_num == 0)) return;
    
    char *src_ptr = (char*) src_buf;
    int row_bytes = col_num * gt_mat->unit_size;
    int dst_pos = (row_start - dst_row_start) * dst_blk_ld;
    dst_pos += col_start - dst_col_start;

    int src_ptr_ld = src_buf_ld * gt_mat->unit_size;
    if (row_num <= MPI_DT_SB_DIM_MAX && col_num <= MPI_DT_SB_DIM_MAX)  
    {
        // Block is small, use predefined data type or define a new 
        // data type to reduce MPI_Accumulate overhead
        int block_dt_id = (row_num - 1) * MPI_DT_SB_DIM_MAX + (col_num - 1);
        MPI_Datatype *dst_dt = &gt_mat->sb_stride[block_dt_id];
        if (col_num == src_buf_ld)
        {
            MPI_Datatype *rcv_dt_ns = &gt_mat->sb_nostride[block_dt_id];
            MPI_Accumulate(src_ptr, 1, *rcv_dt_ns, dst_rank, dst_pos, 1, *dst_dt, op, gt_mat->mpi_win);
        } else {
            if (gt_mat->ld_local == src_buf_ld)
            {
                MPI_Accumulate(src_ptr, 1, *dst_dt, dst_rank, dst_pos, 1, *dst_dt, op, gt_mat->mpi_win);
            } else {
                MPI_Datatype rcv_dt;
                MPI_Type_vector(row_num, col_num, src_buf_ld, gt_mat->datatype, &rcv_dt);
                MPI_Type_commit(&rcv_dt);
                MPI_Accumulate(src_ptr, 1, rcv_dt, dst_rank, dst_pos, 1, *dst_dt, op, gt_mat->mpi_win);
                MPI_Type_free(&rcv_dt);
            }
        }
    } else {   
        // Define a MPI data type to reduce number of request
        MPI_Datatype dst_dt, rcv_dt;
        MPI_Type_vector(row_num, col_num, dst_blk_ld, gt_mat->datatype, &dst_dt);
        MPI_Type_vector(row_num, col_num, src_buf_ld, gt_mat->datatype, &rcv_dt);
        MPI_Type_commit(&dst_dt);
        MPI_Type_commit(&rcv_dt);
        MPI_Accumulate(src_ptr, 1, rcv_dt, dst_rank, dst_pos, 1, dst_dt, op, gt_mat->mpi_win);
        MPI_Type_free(&dst_dt);
        MPI_Type_free(&rcv_dt);
    }
}

// Update (put or accumulate) a block to all related processes using MPI_Accumulate
// This call is not collective, not thread-safe
// [in] gt_mat      : GTMatrix handle
// [in] op          : MPI operation, only support MPI_SUM (accumulate) and MPI_REPLACE (MPI_Put)
// [in] row_start   : 1st row of the source block
// [in] row_num     : Number of rows the source block has
// [in] col_start   : 1st column of the source block
// [in] col_num     : Number of columns the source block has
// [in] *src_buf    : Source buffer
// [in] src_buf_ld  : Leading dimension of the source buffer
// [in] access_mode : Access mode, see GTMatrix_Typedef.h
void GTM_updateBlock(
    GTMatrix_t gt_mat, MPI_Op op, 
    int row_start, int row_num,
    int col_start, int col_num,
    void *src_buf, int src_buf_ld,
    int access_mode
)
{
    // Sanity check
    if ((row_start < 0) || (col_start < 0) ||
        (row_start + row_num > gt_mat->nrows)  ||
        (col_start + col_num > gt_mat->ncols)  ||
        (row_num * col_num == 0)) return;
    
    // Find the processes that contain the requested block
    int s_blk_r, e_blk_r, s_blk_c, e_blk_c;
    int row_end = row_start + row_num - 1;
    int col_end = col_start + col_num - 1;
    for (int i = 0; i < gt_mat->r_blocks; i++)
    {
        if ((gt_mat->r_displs[i] <= row_start) && 
            (row_start < gt_mat->r_displs[i+1])) s_blk_r = i;
        if ((gt_mat->r_displs[i] <= row_end)   && 
            (row_end   < gt_mat->r_displs[i+1])) e_blk_r = i;
    }
    for (int i = 0; i < gt_mat->c_blocks; i++)
    {
        if ((gt_mat->c_displs[i] <= col_start) && 
            (col_start < gt_mat->c_displs[i+1])) s_blk_c = i;
        if ((gt_mat->c_displs[i] <= col_end)   && 
            (col_end   < gt_mat->c_displs[i+1])) e_blk_c = i;
    }
    
    // Update data to each process
    int blk_r_s, blk_r_e, blk_c_s, blk_c_e, need_to_fetch;
    for (int blk_r = s_blk_r; blk_r <= e_blk_r; blk_r++)      // Notice: <=
    {
        int dst_r_s = gt_mat->r_displs[blk_r];
        int dst_r_e = gt_mat->r_displs[blk_r + 1] - 1;
        for (int blk_c = s_blk_c; blk_c <= e_blk_c; blk_c++)  // Notice: <=
        {
            int dst_c_s  = gt_mat->c_displs[blk_c];
            int dst_c_e  = gt_mat->c_displs[blk_c + 1] - 1;
            int dst_rank = blk_r * gt_mat->c_blocks + blk_c;
            getRectIntersection(
                dst_r_s,   dst_r_e, dst_c_s,   dst_c_e,
                row_start, row_end, col_start, col_end,
                &need_to_fetch, &blk_r_s, &blk_r_e, &blk_c_s, &blk_c_e
            );
            assert(need_to_fetch == 1);
            int blk_r_num = blk_r_e - blk_r_s + 1;
            int blk_c_num = blk_c_e - blk_c_s + 1;
            int row_dist  = blk_r_s - row_start;
            int col_dist  = blk_c_s - col_start;
            char *blk_ptr = (char*) src_buf;
            blk_ptr += (row_dist * src_buf_ld + col_dist) * gt_mat->unit_size;
            
            if (access_mode == BLOCKING_ACCESS)
            {
                MPI_Win_lock(MPI_LOCK_SHARED, dst_rank, 0, gt_mat->mpi_win);
                GTM_updateBlockToProcess(
                    gt_mat, dst_rank, op, blk_r_s, blk_r_num, 
                    blk_c_s, blk_c_num, blk_ptr, src_buf_ld
                );
                MPI_Win_unlock(dst_rank, gt_mat->mpi_win);
            }
            
            if (access_mode == BATCH_ACCESS)
            {
                GTM_Req_Vector_t req_vec = gt_mat->req_vec[dst_rank];
                GTM_pushToReqVector(
                    req_vec, op, blk_r_s, blk_r_num, 
                    blk_c_s, blk_c_num, blk_ptr, src_buf_ld
                );
            }
        }
    }
}

// Put a block to the global matrix
void GTM_putBlock(GTM_PARAM)
{
    GTM_updateBlock(
        gt_mat, MPI_REPLACE, 
        row_start, row_num,
        col_start, col_num,
        src_buf, src_buf_ld, BLOCKING_ACCESS
    );
}

// Accumulate a block to the global matrix
void GTM_accumulateBlock(GTM_PARAM)
{
    GTM_updateBlock(
        gt_mat, MPI_SUM, 
        row_start, row_num,
        col_start, col_num,
        src_buf, src_buf_ld, BLOCKING_ACCESS
    );
}

// Add a request to put a block to the global matrix
void GTM_addPutBlockRequest(GTM_PARAM)
{
    GTM_updateBlock(
        gt_mat, MPI_REPLACE, 
        row_start, row_num,
        col_start, col_num,
        src_buf, src_buf_ld, BATCH_ACCESS
    );
}

// Add a request to accumulate a block to the global matrix
void GTM_addAccumulateBlockRequest(GTM_PARAM)
{
    GTM_updateBlock(
        gt_mat, MPI_SUM, 
        row_start, row_num,
        col_start, col_num,
        src_buf, src_buf_ld, BATCH_ACCESS
    );
}

// Start a batch update epoch and allow to submit update requests
void GTM_startBatchUpdate(GTMatrix_t gt_mat)
{
    if (gt_mat->is_batch_getting) return;
    
    for (int i = 0; i < gt_mat->comm_size; i++)
        GTM_resetReqVector(gt_mat->req_vec[i]);
    gt_mat->is_batch_updating = 1;
}

// Execute all update requests in the queues
void GTM_execBatchUpdate(GTMatrix_t gt_mat)
{
    if (gt_mat->is_batch_updating == 0) return;
    
    for (int _dst_rank = gt_mat->my_rank; _dst_rank < gt_mat->comm_size + gt_mat->my_rank; _dst_rank++)
    {    
        int dst_rank = _dst_rank % gt_mat->comm_size;
        GTM_Req_Vector_t req_vec = gt_mat->req_vec[dst_rank];
        
        if (req_vec->curr_size > 0) 
        {
            MPI_Win_lock(MPI_LOCK_SHARED, dst_rank, 0, gt_mat->mpi_win);
            for (int i = 0; i < req_vec->curr_size; i++)
            {
                MPI_Op op      = req_vec->ops[i];
                int blk_r_s    = req_vec->row_starts[i];
                int blk_r_num  = req_vec->row_nums[i];
                int blk_c_s    = req_vec->col_starts[i];
                int blk_c_num  = req_vec->col_nums[i];
                void *blk_ptr  = req_vec->src_bufs[i];
                int src_buf_ld = req_vec->src_buf_lds[i];
                GTM_updateBlockToProcess(
                    gt_mat, dst_rank, op, blk_r_s, blk_r_num, 
                    blk_c_s, blk_c_num, blk_ptr, src_buf_ld
                );
            }
            MPI_Win_unlock(dst_rank, gt_mat->mpi_win);
        }
        
        GTM_resetReqVector(req_vec);
    }
}

// Stop a batch update epoch and disallow to submit update requests
void GTM_stopBatchUpdate(GTMatrix_t gt_mat)
{
    if (gt_mat->is_batch_updating == 0) return;
    gt_mat->is_batch_updating = 0;
}
