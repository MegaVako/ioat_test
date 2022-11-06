#include <linux/pagemap.h>
#include <linux/completion.h>
#include <linux/async_tx.h>
#include <linux/blkdev.h>
#include <linux/slab.h>
#include <linux/crc32c.h>
#include <linux/dmaengine.h>
#include <linux/dma-mapping.h>
#include <linux/module.h>
#include <linux/kernel.h>

#include <asm/apic.h>
#include <asm/set_memory.h>
#include <asm/cpufeature.h>

//#define CACHE_SIZE (36608 << 12)
#define CACHE_SIZE (4096 << 12)

// perf counters stuff
#define IA32_PERF_GLOBAL_CTRL_ENABLE 0x70000000f
#define LLC_EVENT 0x0043412e
#define IA32_PERF_FIXED_CTRL_ENABLE 0x333

// ioat structs
struct async_submit_ctl submit;
addr_conv_t addr_conv[2];
struct dma_chan *chan = NULL;
struct dma_device *ioat_device = NULL;

void callback(void *param) {
    struct completion *cmp = param;
	complete(cmp);
}

void init_perf_counters(void) {
    int cpu;
    wrmsrl(MSR_CORE_PERF_FIXED_CTR_CTRL, IA32_PERF_FIXED_CTRL_ENABLE);
    wrmsrl(MSR_CORE_PERF_GLOBAL_CTRL, IA32_PERF_GLOBAL_CTRL_ENABLE);
    for_each_possible_cpu(cpu) {
        wrmsrl_on_cpu(cpu, MSR_P6_EVNTSEL0, LLC_EVENT);
    }
}

u64 fetch_perf_counters(void) {
    int cpu;
    u64 val, curr_val;
    val = 0;
    for_each_possible_cpu(cpu) {
        rdmsrl_on_cpu(cpu, MSR_IA32_PERFCTR0, &curr_val);
        val += curr_val;
        //printk(KERN_DEBUG "The LLC-Miss on core %i is: %llu\n", cpu, val);
    }
    //printk(KERN_DEBUG "The LLC-Miss on all core is: %llu\n", val);
    return val;
}

void ioat_cp(u64 dma_src, u64 dma_dst, int len) {
    enum dma_ctrl_flags flags;
    struct completion comp;
    struct dma_async_tx_descriptor *tx = NULL;
    dma_cookie_t dma_cookie;
    enum dma_status status;
    unsigned long timeout;

    flags = DMA_CTRL_ACK | DMA_PREP_INTERRUPT;
    init_completion(&comp);

    tx = dmaengine_prep_dma_memcpy(chan, dma_dst, dma_src, len, flags);
    if (IS_ERR_OR_NULL(tx)) {
        pr_err("preparation dma failure.\n");
        return;
    }
    tx->callback = callback;
    tx->callback_param = &comp;

    dma_cookie = dmaengine_submit(tx);
    dma_async_issue_pending(chan);
    timeout = wait_for_completion_timeout(&comp, msecs_to_jiffies(100));
    status = dma_async_is_tx_complete(chan, dma_cookie, NULL, NULL);
    
   	if (timeout == 0) {
		printk(KERN_WARNING "DMA timed out.\n");
	} else if (status != DMA_COMPLETE) {
		printk(KERN_INFO "DMA returned completion status of: %s\n",
            status == DMA_ERROR ? "error" : "in progress");
	} else {
		//printk(KERN_INFO "DMA completed!\n");
	} 
}

void test_memcpy(int src_num_pages) {
    int i;
    char* dst;
    char** src;

    src = kmalloc(src_num_pages * sizeof(char*), GFP_KERNEL);
    dst = kmalloc(PAGE_SIZE, GFP_KERNEL);

    for (i = 0; i < src_num_pages; i++) {
        src[i] = kmalloc(PAGE_SIZE, GFP_KERNEL);
        if (src[i] == NULL) {
            pr_err("test_memcpy src alloc failed, at %d\n", i);
            src_num_pages = i;
            goto out;
        }
    }

    for (i = 0; i < src_num_pages; i++) {
        memcpy(dst, src[i], PAGE_SIZE);
    }
    pr_info("test_memcpy, done copy %ldMB to dst\n", (src_num_pages * PAGE_SIZE) >> 20);

out:
    for (i = 0; i < src_num_pages; i++) {
        kfree(src[i]);
    }
    kfree(src);
    kfree(dst);
}

// assume init_ioat is called
void test_ioat_cp(int src_num_pages) {
    int i;
    char* dst;
    char** src;
    u64 dma_dst;
    u64* dma_src;
    enum dma_ctrl_flags flags;
    struct completion comp;
    struct dma_async_tx_descriptor *tx = NULL;
    dma_cookie_t dma_cookie;
    enum dma_status status;
    unsigned long timeout;

    init_completion(&comp);
    
    dst = kmalloc(PAGE_SIZE, GFP_KERNEL);
    dma_src = kmalloc(src_num_pages * sizeof(u64), GFP_KERNEL);
    src = kmalloc(src_num_pages * sizeof(char*), GFP_KERNEL);

    dma_dst = dma_map_single(ioat_device->dev, dst, PAGE_SIZE, DMA_BIDIRECTIONAL); 
    flags = 0;

    for (i = 0; i < src_num_pages; i++) {
        src[i] = kmalloc(PAGE_SIZE, GFP_KERNEL);
        if (src[i] == NULL) {
            pr_err("test_ioat_cp src alloc failed, at %d\n", i);
            src_num_pages = i;
            goto out;
        }
        dma_src[i] = dma_map_single(ioat_device->dev, src[i], PAGE_SIZE, DMA_BIDIRECTIONAL);
    }

    for (i = 0; i < src_num_pages; i++) {
        if (i == src_num_pages - 1) {
            flags = DMA_CTRL_ACK | DMA_PREP_INTERRUPT;
        }

        tx = dmaengine_prep_dma_memcpy(chan, dma_dst, dma_src[i], PAGE_SIZE, flags);

        if (IS_ERR_OR_NULL(tx)) {
            pr_err("preparation dma failure, at %d.\n", i);
            break;
        }
        tx->callback = callback;
        tx->callback_param = &comp;

        dma_cookie = dmaengine_submit(tx);
    }
    dma_async_issue_pending(chan);
    timeout = wait_for_completion_timeout(&comp, msecs_to_jiffies(50000));
    status = dma_async_is_tx_complete(chan, dma_cookie, NULL, NULL);

   	if (timeout == 0) {
		printk(KERN_WARNING "DMA timed out.\n");
	} else if (status != DMA_COMPLETE) {
		printk(KERN_INFO "DMA returned completion status of: %s\n",
            status == DMA_ERROR ? "error" : "in progress");
	} else {
        pr_info("test_ioat_cp, done copy %ldMB to dst\n", (src_num_pages * PAGE_SIZE) >> 20);
	} 

out:
    for (i = 0; i < src_num_pages; i++) {
        kfree(src[i]);
        dma_unmap_single(ioat_device->dev, dma_src[i], PAGE_SIZE, DMA_BIDIRECTIONAL);
    }
    dma_unmap_single(ioat_device->dev, dma_dst, PAGE_SIZE, DMA_BIDIRECTIONAL);
    
    kfree(src);
    kfree(dst);
    kfree(dma_src);
}

static int init_ioat(void) {
    dma_cap_mask_t mask;

    init_async_submit(&submit, 0, NULL, NULL, NULL, addr_conv);

    dma_cap_zero(mask);
    dma_cap_set(DMA_MEMCPY, mask);

    chan = dma_request_channel(mask, NULL, NULL);

    if (chan == NULL) {
        pr_err("Invalid DMA channel\n");
        return -1;
    }
    else{
        ioat_device = chan->device;
		pr_info("Valid DMA device\n");
    }
	return 0;
}


// len in byte
void touch_buf(char** buf, int num_pages, u64* cycle, u64* cache) {
    int i, j;
    char dummy;
    u64 t1, t2, c1, c2;
    c1 = fetch_perf_counters();
    t1 = rdtsc();
    for (j = 0; j < num_pages; j++) {
        for (i = 0; i < PAGE_SIZE; i++) {
            dummy = buf[j][i];
            dummy += 1;
        }
    }
    t2 = rdtsc();
    c2 = fetch_perf_counters();
    *cycle = t2 - t1;
    *cache = c2 - c1;
    pr_info("=============\n");
    pr_info("[touch_buf] touch: %d pages\n", num_pages);
    pr_info("[touch_buf] touch cycles = %llu\n", *cycle);
    pr_info("[touch_buf] touch cache miss = %llu\n", *cache);
}

void modify_buf(char** buf, int num_pages, u64* cycle, u64* cache) {
    int i, j;
    u64 t1, t2, c1, c2;
    c1 = fetch_perf_counters();
    t1 = rdtsc();
    for (j = 0; j < num_pages; j++) {
        for (i = 0; i < PAGE_SIZE; i++) {
            buf[j][i] += 1;
        }
    }
    t2 = rdtsc();
    c2 = fetch_perf_counters();
    *cycle = t2 - t1;
    *cache = c2 - c1;
    pr_info("=============\n");
    pr_info("[modify_buf] modify: %d pages\n", num_pages);
    pr_info("[modify_buf] modify cycles = %llu\n", *cycle);
    pr_info("[modify_buf] modify cache miss = %llu\n", *cache);
}


static int ioat_test_init(void){
    char** cache_size_buf;
    char* curr_buf;
    int cache_num_pages, i;
    u64 t1, t2, c1, c2;
    u64 cycle_pre_test, cache_pre_test, cycle_post_test, cache_post_test;

	pr_info("ioat test init!\n");
	chan = NULL; // avoid accidentially release a void chan
	init_ioat();

    cache_num_pages = CACHE_SIZE >> 12;
    cache_size_buf = kmalloc(cache_num_pages * sizeof(char*), GFP_KERNEL);

    for (i = 0; i < cache_num_pages; i++) {
        curr_buf = kmalloc(PAGE_SIZE, GFP_KERNEL);
        if (curr_buf == NULL) {
            pr_err("curr_buf alloc failed\n");
            cache_num_pages = i;
            goto out;
        }
        cache_size_buf[i] = curr_buf;
    }
    pr_info("alloc all ok, num_pages: %d, total_size: %dMB\n", cache_num_pages, CACHE_SIZE >> 20);

    modify_buf(cache_size_buf, cache_num_pages, &cycle_pre_test, &cache_pre_test);

    touch_buf(cache_size_buf, cache_num_pages, &cycle_pre_test, &cache_pre_test);
    touch_buf(cache_size_buf, cache_num_pages, &cycle_pre_test, &cache_pre_test);
    touch_buf(cache_size_buf, cache_num_pages, &cycle_pre_test, &cache_pre_test);
    touch_buf(cache_size_buf, cache_num_pages, &cycle_pre_test, &cache_pre_test);
    touch_buf(cache_size_buf, cache_num_pages, &cycle_pre_test, &cache_pre_test);
    touch_buf(cache_size_buf, cache_num_pages, &cycle_pre_test, &cache_pre_test);
    touch_buf(cache_size_buf, cache_num_pages, &cycle_pre_test, &cache_pre_test);
    touch_buf(cache_size_buf, cache_num_pages, &cycle_pre_test, &cache_pre_test);

    c1 = fetch_perf_counters();
    t1 = rdtsc();

    /*
    test_memcpy((2 << 14));
    test_memcpy((2 << 14));
    test_memcpy((2 << 14));
    */
    test_ioat_cp(2 << 14);
    test_ioat_cp(2 << 14);
    test_ioat_cp(2 << 14);

    t2 = rdtsc();
    c2 = fetch_perf_counters();
    pr_info("=============\n");
    pr_info("[test_*] cycles = %llu\n", t2 - t1);
    pr_info("[test_*] cache miss = %llu\n", c2 - c1);

    touch_buf(cache_size_buf, cache_num_pages, &cycle_post_test, &cache_post_test);

    pr_info("Diff last cycle      = %lld\n", (int64_t)cycle_post_test - (int64_t)cycle_pre_test);
    pr_info("Diff last cache miss = %lld\n", (int64_t)cache_post_test - (int64_t)cache_pre_test);

    
out:
    for (i = 0; i < cache_num_pages; i++) {
        kfree(cache_size_buf[i]);
    }
    kfree(cache_size_buf);
	return 0;
}
    
static void ioat_test_exit(void){
	if (chan) {
		dma_release_channel(chan);
	}
	pr_info("ioat test exits!\n");
}

module_init(ioat_test_init);
module_exit(ioat_test_exit);
MODULE_LICENSE("GPL");
