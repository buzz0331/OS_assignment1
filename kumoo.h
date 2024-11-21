#define ADDR_SIZE 16
#define MAX_PROCESSES 10

struct pcb *current;
unsigned short *pdbr;
//physical memory의 base / swap space의 base
char *pmem, *swaps;
//각각 physical memory와 swap space안의 page frame의 개수
int pfnum, sfnum;

//처음에 실행시킬 process 개수
int num_of_processes = 0;

//현재 실행중인 process 개수
int process_num = 0;

//page가 mapping되는 순서 (swap-out 시킬 page를 결정해주는 역할!)
int sequential_page = 2;
int eviction_sequence = 2;

void ku_dump_pmem(void);
void ku_dump_swap(void);

int swap_out();

struct pcb{
	unsigned short pid;
	FILE *fd;
	unsigned short *pgdir;
	unsigned short va_start;
    unsigned short va_size;
};

struct pcb processes[MAX_PROCESSES];
int valid_proc[MAX_PROCESSES];

int *freelist;
int *swapspace;



void ku_freelist_init(){
    freelist = (int *)malloc(sizeof(int) * pfnum);
    for(int i=0; i<pfnum;i++){
        freelist[i] = 0;
    }

//    for (int i = 0; i < pfnum; i++) {
//        physical_memory.base[i] = 0;
//    }

    swapspace = (int *)malloc(sizeof(int) * sfnum);
    for(int i=0; i<sfnum; i++){
        swapspace[i] = 0;
    }

}

int ku_proc_init(int argc, char *argv[]){
    if(argc != 2){
        printf("인자의 개수가 옳지 않습니다.");
        return 1;
    }

    for(int i=0; i<MAX_PROCESSES; i++){
        valid_proc[i] = 0;
    }

    FILE *input_file = fopen(argv[1],"r");
    if(input_file == NULL){
        perror("파일 열기 실패!");
        return 1;
    }

    char instruction[20];
    while (fgets(instruction, sizeof(instruction), input_file) != NULL) {

        //현재 읽은 instruction을 실행할 process의 id
        int process_id;
        char filename[12];

        sscanf(instruction, "%d %s", &process_id, filename);

        struct pcb *process = &processes[process_id];

        //유효한 process 표시
        valid_proc[process_id] = 1;

        process->pid = process_id;
        process->fd = fopen(filename, "r");
        if(process->fd == NULL){
            perror("파일 열기 실패!");
            continue;
        }

        //proc.txt에서 2줄 읽어서 virtual address의 시작주소와 크기 PCB에 저장
        char d;
        fscanf(process->fd, "%c", &d);
        if (fscanf(process->fd, "%hd %hd", &process->va_start, &process->va_size) == EOF) {
            /* Invaild file format */
            return 1;
        }
//        printf("%d %d", process->va_start, process->va_size);

        //page directory 생성
//        process.pgdir = (unsigned short *) malloc(sizeof(unsigned short)<<5);
        process->pgdir = (unsigned short *) (pmem + (process_id << 6));

        if(process->pgdir == NULL){
            perror("페이지 디렉터리 메모리 할당 실패!");
            return 1;
        }

        //page directory 초기화
        for(int i=0; i<(1<<5); i++){
            process->pgdir[i] = 0;
        }

        //freelist update
        freelist[process_id] = 1;

        num_of_processes++;
        process_num++;
    }

    // 입력 파일 닫기
    fclose(input_file);

    return 0;
}

//다음에 어떤 process를 실행할지 선정(RR) -> pdbr, current update
int ku_scheduler(unsigned short process_id){
    int id = process_id;
    /* No processes */
    if(process_num == 0){
        return 1;
    }

    //가장 처음에 process_id에 10이 들어왔을 때, 첫번째 process부터 시작
    if(id == 10){
        id = 0;
    }
    else{
        do{
            id = (id+1)%num_of_processes;
        }while(valid_proc[id] == 0);

    }

    current = &processes[id];
    pdbr = current->pgdir;

    return 0;
}

//va: page fault가 발생한 virtual address
int ku_pgfault_handler(unsigned short va){

    //segmentation fault!!
    if(current->va_start > va || current->va_start  + current->va_size <= va){
        return 1;
    }

    int pd_index = (va & 0xFFC0) >> 11;
    unsigned short *pde = pdbr + pd_index;

    int pgtable_PFN = -1;

    if(!(*pde & 0x1)){
        //page directory에서 page fault 발생!!

        //page table에 할당할 PFN 찾음
        for(int i = 0; i< pfnum; i++){
            if(freelist[i] == 0) {
                pgtable_PFN = i;
                break;
            }
        }
    } else{
        //page table에서 page fault 발생!!
        pgtable_PFN = (*pde & 0xFFF0) >> 4;
    }

    if (pgtable_PFN == -1) {
        //swap out 시작!
        pgtable_PFN = swap_out();
        if(pgtable_PFN == -1)
            //pmem에 page directory뿐이라 swap out 시킬 수 없는 경우
            return 1;
    }
    freelist[pgtable_PFN] = 1;



    unsigned short *ptbr = (unsigned short*)(pmem + (pgtable_PFN << 6));
    int pt_index = (va & 0x07C0) >> 6;


    // 사용할 PFN을 찾음 (sequential search)
    int free_PFN = -1;
    for (int i = 0; i < pfnum; i++) {
        if (freelist[i] == 0) { // 페이지가 할당되지 않은 경우
            free_PFN = i;
            break;
        }
    }

    if (free_PFN == -1) {
        //swap out 시작!
        free_PFN = swap_out();
        if(free_PFN == -1)
            //pmem에 page directory뿐이라 swap out 시킬 수 없는 경우
            return 1;
    }



    unsigned short *pte = ptbr + pt_index;
    if((*pte & 0xFFFC) != 0){
        //swap-in 구현
        int swap_PFN = (*pte & 0xFFFC);

        //swap space에 있는 page physical memory에 복사
        for(int i=0; i<64; i++){
            int pmem_index = (free_PFN << 6) + i;
            int swap_index = (swap_PFN << 6) + i;
            pmem[pmem_index] = swaps[swap_index];
        }
        swapspace[swap_PFN] = 0;

        //swap out된 page의 dirty bit는 1일 것이므로 pte의 dirty bit도 1을 넣어줌
        *pte = (free_PFN << 4) | 0x3;

    }else{
        // 처음 load한 page를 0으로 초기화
        for(int i = (free_PFN << 6); i < (free_PFN << 6) + 64; i++)
            pmem[i] = 0;

        *pte = (free_PFN << 4) | 0x1;
    }

    // 페이지 디렉토리 업데이트 작업 수행//
    *pde = ((pgtable_PFN << 4) | 0x0001);
//    current->pgdir[pd_index] = PDE;

    // freelist 업데이트
    freelist[free_PFN] = sequential_page;
    sequential_page++;

    // 페이지 폴트 처리가 성공
    return 0;
}
int ku_proc_exit(unsigned short pid){
    if (pid < 0 || pid >= MAX_PROCESSES || !freelist[pid]) {
        // 유효하지 않은 PID인 경우
        return 1;
    }

    struct pcb *process = &processes[pid];

    fclose(process->fd);
    valid_proc[pid] = 0;

    int pgtable_PFN;
    unsigned short *pde, *ptbr, *pte;

    for (int index = 0; index < (1 << 5); index++) {
        // 페이지 디렉터리 엔트리(PDE) 인덱스 계산
//        pd_index = (va & 0xFFC0) >> 11;
        pde = process->pgdir + index;

        // 해당 페이지 테이블이 존재하는지 확인
        if (*pde & 0x1) {
            // 페이지 테이블 엔트리(PTE)의 PFN을 가져옴
            pgtable_PFN = (*pde & 0xFFF0) >> 4;

            ptbr = (unsigned short *)(pmem + (pgtable_PFN << 6));

            for(int pt_index=0; pt_index< (1 << 5); pt_index++){
                pte = ptbr + pt_index;

                if(*pte & 0x1){
                    // 페이지 테이블 엔트리(PTE)의 사용 여부를 해제하고 freelist 업데이트
                    int PFN = (*pte & 0xFFF0) >> 4;
                    unsigned short * page = (unsigned short *)(pmem + (PFN << 6));
                    unsigned short *entry;
                    for(int i=0; i< (1<<5); i++){
                        //mapping되어있던 page들도 free
                        entry = page + i;
                        *entry = 0;
                    }
                    freelist[PFN] = 0;
                    *pte = 0;
                }
            }

            freelist[pgtable_PFN] = 0;
            *pde = 0;
        }
    }

    // 해당 PID를 가지고 있는 PCB 초기화
    process->pid = 0;
    process->fd = NULL;
    process->pgdir = NULL;
    process->va_start = 0;
    process->va_size = 0;

    // 해당 pid에 해당하는 프로세스가 종료되었으므로 freelist 업데이트
    freelist[pid] = 0;
    process_num--;

    return 0;
}

int swap_out(){
    //pmem에서 swapout시킬 PFN
    int swapout_PFN = -1;

    //swap out 시킬 page 하나를 free list에서 찾는다.
    for(int i=0; i<pfnum; i++){
        if(freelist[i] == eviction_sequence){ //가장 처음 들어온 page를 찾아서 evict
            swapout_PFN = i;
            eviction_sequence++;
            break;
        }
    }

    if(swapout_PFN == -1){
        //PFN이 모두 page directory로 가득차있어서 swap out이 불가능!
        return -1;
    }

    //해당 PFN과 mapping되어 있는 pte search
    for(int processNum=0; processNum<num_of_processes; processNum++){
        //현재 실행중인 process만 수행
        if(valid_proc[processNum] == 1){
            struct pcb *process = &processes[processNum];

            int pgtable_PFN;
            unsigned short *pde, *ptbr, *pte;

            for (int index = 0; index < (1 << 5); index++) {
                // 페이지 디렉터리 엔트리(PDE) 인덱스 계산

                pde = process->pgdir + index;

                // 해당 페이지 테이블이 존재하는지 확인
                if (*pde & 0x1) {
                    // 페이지 테이블 엔트리(PTE)의 PFN을 가져옴
                    pgtable_PFN = (*pde & 0xFFF0) >> 4;
                    ptbr = (unsigned short *)(pmem + (pgtable_PFN << 6));

                    for(int pt_index=0; pt_index< (1 << 5); pt_index++){
                        pte = ptbr + pt_index;

                        if(*pte & 0x1){
                            int PFN = (*pte & 0xFFF0) >> 4;

                            if(PFN == swapout_PFN){
                                //swapout하려는 PFN과 mapping된 pte를 찾은 경우
                                if(*pte & 0x2){
                                    //dirty bit가 1이라면 swap space로 이동
                                    int swap_PFN = -1;
                                    for(int j=0; j < sfnum; j++){
                                        if(swapspace[j] == 0) {
                                            //swap공간에서 비어있는 PFN 하나를 찾음
                                            swap_PFN = j;
                                            break;
                                        }
                                    }
                                    for(int index = 0; index < 64; index++){
                                        //pmem에 있는 page를 swap space에 복사
                                        int pmem_index = (swapout_PFN << 6) + index;
                                        int swap_index = (swap_PFN << 6) + index;
                                        swaps[swap_index] = pmem[pmem_index];
                                        pmem[pmem_index] = 0;
                                    }
                                    *pte = (swap_PFN << 2) | 0x2;

                                    swapspace[swap_PFN] = 1;

                                    return swapout_PFN;
                                }else{
                                    //dirty bit가 0이라면 pte 0으로 만들고 mapping되어 있는 page도 0으로 만듬
                                    *pte = 0x0;

                                    //현재 pde에 mapping된 page table의 모든 pte가 0일 경우 pde도 0으로 초기화
                                    for(int i = 0; i< (1<<5); i++){
                                        if(*pte != 0){
                                            break;
                                        }
                                        if(i== (1<<5)-1){
                                            //현재 mapping된 page table의 모든 pte가 0인 경우
                                            *pde = 0x0;
                                            freelist[PFN] = 0;
                                        }
                                    }

                                    for(int index = 0; index < 64; index++){
                                        int pmem_index = (swapout_PFN << 6) + index;
                                        pmem[pmem_index] = 0;
                                    }

                                    freelist[swapout_PFN] = 0;

                                    return swapout_PFN;
                                }
                            }
                        }
                    }
                }
            }

        }
        else{ //실행중이지 않은 process면 넘어감
            continue;
        }
    }
    return 0;
}