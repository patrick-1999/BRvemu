#include "rvemu.h"
#define FILE_PATH "cache_file" // 用于模拟磁盘存储

int miss_counter = 0;
int hit_counter = 0;



// priority_queue_t pq;

double sigmoid(double x) {
    return 1.0 / (1.0 + exp(-x));
}

void pq_insert_update(priority_queue_t* pq, cache_item_t* item);
bool pq_contains(priority_queue_t* pq, u64 pc);
bool compare_priority(cache_item_t* a, cache_item_t* b);
void pq_init(priority_queue_t* pq);



void prepare_file() {
    int fd = open(FILE_PATH, O_CREAT | O_RDWR, 0666);
    if (fd < 0) {
        perror("Failed to create file");
        exit(EXIT_FAILURE);
    }
    // 扩展文件大小
    if (ftruncate(fd, CACHE_SIZE) != 0) {
        perror("Failed to resize file");
        close(fd);
        exit(EXIT_FAILURE);
    }
    close(fd);
}


#define sys_icache_invalidate(addr, size) \
  __builtin___clear_cache((char *)(addr), (char *)(addr) + (size));

static u64 hash(u64 pc) {
    return pc % CACHE_ENTRY_SIZE;
}




cache_t *new_cache() {
    prepare_file();
    // 打开文件并映射到内存
    int fd = open(FILE_PATH, O_RDWR);
    if (fd < 0) {
        perror("Failed to open file");
        exit(EXIT_FAILURE);
    }
    cache_t *cache = (cache_t *)calloc(1, sizeof(cache_t));
    cache->jitcode = (u8 *)mmap(NULL, CACHE_SIZE, PROT_READ | PROT_WRITE | PROT_EXEC,
                          MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
    return cache;
}

#define MAX_SEARCH_COUNT 32
#define CACHE_HOT_COUNT  16

#define CACHE_IS_HOT (cache->table[index].hot >= CACHE_HOT_COUNT)

u8 *cache_lookup(cache_t *cache, u64 pc) {
    assert(pc != 0);

    u64 index = hash(pc);

    while (cache->table[index].pc != 0) {
        if (cache->table[index].pc == pc) {
            if (CACHE_IS_HOT)
                return cache->jitcode + cache->table[index].offset;
            break;
        }

        index++;
        index = hash(index);
    }

    return NULL;
}

static FORCE_INLINE u64 align_to(u64 val, u64 align) {
    if (align == 0)
        return val;
    return (val + align - 1) & ~(align - 1);
}

u8 *cache_add(cache_t *cache, u64 pc, u8 *code, size_t sz, u64 align) {
    // 将cache的对齐到页面的大小
    size_t page_size = sysconf(_SC_PAGESIZE);
    align = page_size;
    cache->offset = align_to(cache->offset, align);
    assert(cache->offset + sz <= CACHE_SIZE);

    u64 index = hash(pc);
    u64 search_count = 0;
    while (cache->table[index].pc != 0) {
        if (cache->table[index].pc == pc) {
            break;
        }

        index++;
        index = hash(index);

        assert(++search_count <= MAX_SEARCH_COUNT);
    }

    memcpy(cache->jitcode + cache->offset, code, sz);
    cache->table[index].pc = pc;
    cache->table[index].offset = cache->offset;
    cache->table[index].last_logical_time = cache->logical_time;
    cache->offset += sz;
    sys_icache_invalidate(cache->jitcode + cache->table[index].offset, sz);
    return cache->jitcode + cache->table[index].offset;
}

bool cache_hot(cache_t *cache, u64 pc) {
    u64 index = hash(pc);
    u64 search_count = 0;
    while (cache->table[index].pc != 0) {
        if (cache->table[index].pc == pc) {
            cache->table[index].hot = MIN(++cache->table[index].hot, CACHE_HOT_COUNT);
            return CACHE_IS_HOT;
        }

        index++;
        index = hash(index);

        assert(++search_count <= MAX_SEARCH_COUNT);
    }

    cache->table[index].pc = pc;
    cache->table[index].hot = 1;
    return false;
}



// // 如果是cache中代码的执行，那肯定是编译之后的代码
void cache_exec(cache_t *cache, u64 pc){

    // 首先需要检查执行的代码是不是存储在DRAM中，暂时先不实现
    // 如果不在DRAM中需要将对应的代码块调度到磁盘中，并且在磁盘空间不足的时候触发对应的替换逻辑
    // 更新元数据信息,记录当前的逻辑时间和重用距离
    assert(pc != 0);
    u64 index = hash(pc);
    while (cache->table[index].pc != 0) {
        if (cache->table[index].pc == pc) {
            if (CACHE_IS_HOT){
                // 只有检查出这个基本快是热的才会实际执行采用编译执行的方式
                // printf("pc:%lx\n",pc);
                cache->table[index].hot++;
                // printf("last_logical_time:%lx\n",cache->table[index].last_logical_time);
                // printf("logical_time:%lx\n",cache->logical_time);
                cache->table[index].reused_distance = cache->logical_time - cache->table[index].last_logical_time;
                cache->table[index].last_logical_time = cache->logical_time;
                // 采用sigmoid来解决超出double表示范围的问题
                cache->table[index].priority += sigmoid(exp(-1*(cache->table[index].reused_distance/QUEUE_MAX_SIZE)));
                cache->table[index].period_priority += sigmoid(exp(-1*(cache->table[index].reused_distance/QUEUE_MAX_SIZE)));
                // printf("reused_distance:%lx\n",cache->table[index].reused_distance);

                // 更新优先级队列
                pq_insert_update(&cache->pq, &cache->table[index]);
                // pq_print(&cache->pq);
            }
            break;
        }
        index++;
        index = hash(index);
    }


    

}



// 初始化优先级队列
void pq_init(priority_queue_t* pq) {
    pq->size = 0;
    for (int i = 0; i < QUEUE_MAX_SIZE; i++) {
        pq->items[i] = NULL;
    }
}

// 比较两个元素的优先级（返回 true 表示 a 的优先级比 b 高）
bool compare_priority(cache_item_t* a, cache_item_t* b) {
    if (!a || !b) return false;  // 空指针保护
    u64 priority_a = a->priority;
    u64 priority_b = b->priority;
    return priority_a > priority_b;  // 优先级按大到小排序
}

// 检查队列中是否已经存在某个 pc
bool pq_contains(priority_queue_t* pq, u64 pc) {
    for (int i = 0; i < pq->size; i++) {
        if (pq->items[i] && pq->items[i]->pc == pc) {
            return true;  // 找到相同的 pc
        }
    }
    return false;
}

// 插入元素到优先级队列,如果已经存在就实现更新
void pq_insert_update(priority_queue_t* pq, cache_item_t* item) {
    if (!pq || !item) return;

// 如果不是已经存在于队列需要插入

    if (!pq_contains(pq, item->pc)) {

        if (pq->size < QUEUE_MAX_SIZE) {
            // 队列未满，直接插入到最后
            pq->items[pq->size] = item;
            pq->size++;
            printf("size:%d PC:%lx\n",pq->size,item->pc);
        } else {
            // 队列已满，检查是否需要替换优先级最低的元素
            int min_index = 0;
            for (int i = 0; i < pq->size; i++) {
                if (compare_priority(pq->items[min_index], pq->items[i])) {
                    min_index = i;
                }
            }
            // 找到优先级队列中最末尾的块将其替换
            
            printf("cache miss! \n");
            miss_counter++;
            printf("miss_counter :%d\n",miss_counter);
            printf("add cache block PC:%lx \n",item->pc);
            printf("removed cache block PC:%lx \n",pq->items[min_index]->pc);
            pq_print(pq);

            // 将原本的cache块标志位职位在SSD中
            pq->items[min_index]->inDRAM = false;
            pq->items[min_index] = item;
            // 将新加入的块的标志位职位在DRAM中
            pq->items[min_index]->inDRAM = true;
            
        }
       
    }else{
        hit_counter++;
        printf("cache hit :%d\n",hit_counter);
        // printf("block PC:%lx \n",item->pc);
    }


    

    // 按优先级重新排序
    for (int i = 0; i < pq->size - 1; i++) {
        for (int j = i + 1; j < pq->size; j++) {
            if (compare_priority(pq->items[j], pq->items[i])) {
                cache_item_t* temp = pq->items[i];
                pq->items[i] = pq->items[j];
                pq->items[j] = temp;
            }
        }
    }
}

void attenuation(priority_queue_t* pq){
    double alpha = 0.95;
    for(int i=0;i<pq->size;i++){
        pq->items[i]->priority = (1-alpha)*(pq->items[i]->period_priority) + alpha*(pq->items[i]->priority);
        pq->items[i]->period_priority = 0;
    }
}


// 打印优先级队列 
void pq_print(priority_queue_t* pq) {
    printf("Priority Queue (size = %d):\n", pq->size);
    for (int i = 0; i < pq->size; i++) {
        printf("  Item %d: pc = %lx ,hot = %d, reused_distance = %lx, priority = %f ,period_pri = %f \n",
               i, pq->items[i]->pc,(int)pq->items[i]->hot, pq->items[i]->reused_distance,
               pq->items[i]->priority,pq->items[i]->period_priority);
    }
}



