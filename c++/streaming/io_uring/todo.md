# io_uring Remaining Work

1. Regression test: timed receive timeout/cancel association
   Add a targeted test verifying that io_uring timed receives correctly associate their cancellation with the right SQE/CQE when a timeout fires concurrently with completion.
