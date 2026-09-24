#ifndef BOOK_WORKER_H_
#define BOOK_WORKER_H_

#include <functional>

// Runs book jobs (library scan, opening a book, loading/paginating a chapter, saving the
// reading position) off the input task. A short-lived task is created per burst of work and
// exits when idle, so its internal-RAM stack only exists while books are being processed.
// One job runs at a time; a job submitted while another runs replaces any job still waiting
// (latest wins), which is what rapid page turns want.
namespace book_worker {

// Returns false only if the worker task could not be created (the job is dropped).
bool Submit(std::function<void()> job);

}  // namespace book_worker

#endif  // BOOK_WORKER_H_
