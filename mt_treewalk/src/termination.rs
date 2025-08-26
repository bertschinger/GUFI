use std::sync::atomic::Ordering;
use std::sync::atomic::{AtomicBool, AtomicUsize};

pub enum TermState {
    Idle,
    Active,
}

pub struct SafraTerminator {
    __token_state: AtomicBool,
    token_holder: AtomicUsize,
    global_done: AtomicBool,
    thread_count: usize,
}

impl SafraTerminator {
    pub fn new(thread_count: usize) -> Self {
        SafraTerminator {
            __token_state: AtomicBool::new(true),
            token_holder: AtomicUsize::new(0),
            global_done: AtomicBool::new(false),
            thread_count,
        }
    }

    fn is_done(&self) -> bool {
        self.global_done.load(Ordering::Relaxed)
    }

    fn get_token_state(&self) -> TermState {
        match self.__token_state.load(Ordering::Relaxed) {
            true => TermState::Active,
            false => TermState::Idle,
        }
    }

    fn set_token_state(&self, state: TermState) {
        match state {
            TermState::Idle => self.__token_state.store(false, Ordering::Relaxed),
            TermState::Active => self.__token_state.store(true, Ordering::Relaxed),
        }
    }

    pub fn check_termination(&self, thread_id: usize, thread_state: TermState) -> bool {
        assert!(thread_id < self.thread_count);

        if self.is_done() {
            return true;
        }

        // Control Thread
        if thread_id == 0 && self.token_holder.load(Ordering::Acquire) == 0 {
            match self.get_token_state() {
                TermState::Idle => {
                    self.global_done.store(true, Ordering::Relaxed);
                    return true;
                }
                TermState::Active => {
                    self.set_token_state(TermState::Idle);
                }
            }
        }

        // Non-Control Thread
        if self.token_holder.load(Ordering::Acquire) == thread_id {
            match thread_state {
                TermState::Idle => {}
                TermState::Active => {
                    self.set_token_state(TermState::Active);
                }
            }
            self.pass_token(thread_id);
        }
        return false;
    }

    fn pass_token(&self, thread_id: usize) {
        self.token_holder
            .store((thread_id + 1) % self.thread_count, Ordering::Release);
    }
}
