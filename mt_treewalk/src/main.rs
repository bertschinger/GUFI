use std::path::{Path, PathBuf};

use clap::Parser;
use crossbeam::deque::{Steal, Stealer, Worker};

mod termination;

use termination::*;

#[derive(Parser, Debug)]
struct Args {
    /// The root to start traversing from.
    path: Option<String>,

    /// Number of threads.
    #[arg(short = 'j', long, default_value_t = 4)]
    n_threads: usize,
}

struct State<'a> {
    term: SafraTerminator,
    stealers: &'a [Stealer<PathBuf>],
}

#[derive(Debug)]
struct WorkerStats {
    n_files: u64,
    n_dirs: u64,
    n_errors: u64,
}

impl WorkerStats {
    fn new() -> Self {
        Self {
            n_files: 0,
            n_dirs: 0,
            n_errors: 0,
        }
    }
}

fn main() {
    let args = Args::parse();

    let root = match args.path {
        Some(path) => path,
        None => ".".to_string(),
    };
    let root = PathBuf::from(root);

    treewalk_mt(root, args.n_threads);
}

fn treewalk_mt(root: PathBuf, n: usize) {
    let mut workers: Vec<Worker<PathBuf>> = Vec::new();
    let mut stealers: Vec<Stealer<PathBuf>> = Vec::new();

    for _ in 0..n {
        let worker = Worker::new_fifo();
        stealers.push(worker.stealer());
        workers.push(worker);
    }

    workers[0].push(root);

    let state = State {
        term: SafraTerminator::new(n),
        stealers: &stealers,
    };

    std::thread::scope(|s| {
        let handles: Vec<_> = (0..n)
            .map(|i| {
                let worker = workers.pop().unwrap();
                let state = &state;
                s.spawn(move || worker_main(&worker, i, state))
            })
            .collect();
        let results = handles.into_iter().map(|t| t.join().unwrap());

        let mut total = WorkerStats::new();

        for result in results {
            total.n_files += result.n_files;
            total.n_dirs += result.n_dirs;
            total.n_errors += result.n_errors;
        }

        println!("Files: {}", total.n_files);
        println!("Directories: {}", total.n_dirs);
        println!("Errors: {}", total.n_errors);
    });
}

fn worker_main(w: &Worker<PathBuf>, id: usize, state: &State) -> WorkerStats {
    let mut stats = WorkerStats::new();
    let mut termination_state = TermState::Idle;

    loop {
        match find_task(w, state.stealers) {
            Some(path) => {
                termination_state = TermState::Active;
                process_directory(&path, w, &mut stats);
            }
            None => {
                if state.term.check_termination(id, termination_state) {
                    break;
                };
                termination_state = TermState::Idle;
            }
        };
    }

    stats
}

fn find_task(local: &Worker<PathBuf>, stealers: &[Stealer<PathBuf>]) -> Option<PathBuf> {
    if let Some(path) = local.pop() {
        return Some(path);
    }

    for i in 0..stealers.len() {
        if let Steal::Success(path) = stealers[i].steal() {
            return Some(path);
        };
    }

    None
}

fn process_directory(path: &Path, w: &Worker<PathBuf>, stats: &mut WorkerStats) {
    let Ok(dir) = std::fs::read_dir(path) else {
        stats.n_errors += 1;
        return;
    };

    for ent in dir {
        let Ok(ent) = ent else {
            stats.n_errors += 1;
            continue;
        };

        let Ok(ty) = ent.file_type() else {
            stats.n_errors += 1;
            continue;
        };

        if ty.is_dir() {
            stats.n_dirs += 1;
            w.push(ent.path());
        } else {
            stats.n_files += 1;
        }
    }
}
