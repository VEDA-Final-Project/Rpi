#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/gpio.h>
#include <linux/interrupt.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/poll.h>
#include <linux/wait.h>
#include <linux/jiffies.h>
#include <linux/device.h>
#include <linux/err.h>

#define DEVICE_NAME "vms_encoder"

// 라즈베리 파이 4 (Base 512) 기준 핀 매핑
#define GPIO_CLK 517  // 512 + BCM 5
#define GPIO_DT  518  // 512 + BCM 6
#define GPIO_SW  538  // 512 + BCM 26

static int major_number;
static int event_val = 0;
static int last_clk = -1;
static int irq_clk, irq_sw;
static unsigned long last_sw_jiffies = 0; // 스위치 디바운싱용

static struct class *vms_class = NULL;
static struct device *vms_device = NULL;

// Qt QSocketNotifier 및 블로킹 read를 위한 대기 큐
static DECLARE_WAIT_QUEUE_HEAD(enc_wait_queue);

// --- [인터럽트] 로터리 엔코더 회전 감지 ---
static irqreturn_t enc_irq_handler(int irq, void *dev_id) {
    int clk = gpio_get_value(GPIO_CLK);
    if (clk != last_clk) {
        event_val = (gpio_get_value(GPIO_DT) != clk) ? 1 : -1;
        wake_up_interruptible(&enc_wait_queue); // 대기 중인 프로세스 깨우기
    }
    last_clk = clk;
    return IRQ_HANDLED;
}

// --- [인터럽트] 스위치 클릭 감지 (250ms 디바운싱 적용) ---
static irqreturn_t sw_irq_handler(int irq, void *dev_id) {
    if (time_after(jiffies, last_sw_jiffies + msecs_to_jiffies(250))) {
        event_val = 100;
        wake_up_interruptible(&enc_wait_queue);
        last_sw_jiffies = jiffies;
    }
    return IRQ_HANDLED;
}

// --- [파일 연산] 데이터 읽기 ---
static ssize_t dev_read(struct file *filep, char *buffer, size_t len, loff_t *offset) {
    int ret;
    int current_val;

    if (event_val == 0) {
        // Non-blocking 모드 (Qt QSocketNotifier)
        if (filep->f_flags & O_NONBLOCK) {
            return -EAGAIN;
        }
        // Blocking 모드 (cat 명령어 등)
        ret = wait_event_interruptible(enc_wait_queue, event_val != 0);
        if (ret) return ret;
    }

    current_val = event_val;
    event_val = 0;

    if (copy_to_user(buffer, &current_val, sizeof(int))) {
        return -EFAULT;
    }
    return sizeof(int);
}

// --- [파일 연산] 이벤트 폴링 (Qt 연동 핵심) ---
static __poll_t dev_poll(struct file *filep, poll_table *wait) {
    __poll_t mask = 0;
    poll_wait(filep, &enc_wait_queue, wait);
    
    if (event_val != 0) {
        mask |= EPOLLIN | EPOLLRDNORM;
    }
    return mask;
}

static struct file_operations fops = { 
    .read = dev_read,
    .poll = dev_poll 
};

// --- [초기화] 모듈 적재 ---
static int __init vms_init(void) {
    int ret;
    
    // 1. 문자 디바이스 등록 및 메이저 번호 할당
    major_number = register_chrdev(0, DEVICE_NAME, &fops);
    if (major_number < 0) return major_number;

    // 2. 장치 노드 자동 생성 (/dev/vms_encoder)
    // 주의: 커널 6.4 이상에서 빌드 에러 발생 시 vms_class = class_create("vms_class"); 로 수정하세요.
    vms_class = class_create("vms_class");
    if (IS_ERR(vms_class)) {
        unregister_chrdev(major_number, DEVICE_NAME);
        return PTR_ERR(vms_class);
    }
    
    vms_device = device_create(vms_class, NULL, MKDEV(major_number, 0), NULL, DEVICE_NAME);
    if (IS_ERR(vms_device)) {
        class_destroy(vms_class);
        unregister_chrdev(major_number, DEVICE_NAME);
        return PTR_ERR(vms_device);
    }

    // 3. GPIO 핀 요청 및 입력 모드 설정
    gpio_request(GPIO_CLK, "VMS_CLK");
    gpio_request(GPIO_DT, "VMS_DT");
    gpio_request(GPIO_SW, "VMS_SW");

    gpio_direction_input(GPIO_CLK);
    gpio_direction_input(GPIO_DT);
    gpio_direction_input(GPIO_SW);

    // 4. 인터럽트 핸들러 등록
    irq_clk = gpio_to_irq(GPIO_CLK);
    ret = request_irq(irq_clk, enc_irq_handler, IRQF_TRIGGER_RISING | IRQF_TRIGGER_FALLING, "vms_irq_enc", NULL);
    
    irq_sw = gpio_to_irq(GPIO_SW);
    ret = request_irq(irq_sw, sw_irq_handler, IRQF_TRIGGER_FALLING, "vms_irq_sw", NULL);

    printk(KERN_INFO "VMS Driver Loaded! Auto-created /dev/%s\n", DEVICE_NAME);
    return 0;
}

// --- [종료] 모듈 제거 ---
static void __exit vms_exit(void) {
    // 1. 장치 노드 삭제 (생성의 역순)
    device_destroy(vms_class, MKDEV(major_number, 0));
    class_destroy(vms_class);

    // 2. 인터럽트 및 GPIO 해제
    free_irq(irq_clk, NULL);
    free_irq(irq_sw, NULL);
    gpio_free(GPIO_CLK); 
    gpio_free(GPIO_DT); 
    gpio_free(GPIO_SW);
    
    // 3. 문자 디바이스 등록 해제
    unregister_chrdev(major_number, DEVICE_NAME);
    printk(KERN_INFO "VMS Driver Unloaded!\n");
}

module_init(vms_init); 
module_exit(vms_exit);
MODULE_LICENSE("GPL");