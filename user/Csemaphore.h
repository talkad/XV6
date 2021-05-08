struct counting_semaphore{
    int value;
    int binSemA;
    int binSemB;
};

void csem_down(struct counting_semaphore *sem);
void csem_up(struct counting_semaphore *sem);
int csem_alloc(struct counting_semaphore *sem, int initial_value);
void csem_free(struct counting_semaphore *sem);