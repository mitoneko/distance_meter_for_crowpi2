#define DEBUG 1
#include <linux/init.h>
#include <linux/module.h>
#include <linux/types.h>
#include <linux/ctype.h>
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/sched.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/gpio/consumer.h>
#include <linux/interrupt.h>
#include <linux/timer.h>
#include <linux/delay.h>
#include <linux/ktime.h>
#include <linux/mutex.h>
#include <asm/current.h>
#include <asm/uaccess.h>

#define DRIVER_NAME "Distance_Meter"

static const unsigned int MINOR_BASE = 0; // udev minor番号の始まり
static const unsigned int MINOR_NUM = 1;  // udev minorの個数
// デバイス全域で使用する変数達
// distance_probeでメモリ確保する。
struct distance_device_info {
    unsigned int major; // udev major番号
    struct cdev cdev;
    struct class *class;
    struct gpio_desc *trig_gpio;
    struct gpio_desc *echo_gpio;
    ktime_t echo_start;
    int echo_length; // 単位:ns 信号幅は、ハードウェア定義により32ms以下である。
    bool is_timer_on;
    struct timer_list timer_for_measure;
    unsigned long measure_span_jiffies; // 鳴動時間(単位:jiffies) 0で永遠
    struct timer_list timer_for_triger_stop;
};

// /dev/distance配下のアクセス関数

static int distance_open(struct inode *inode, struct file *file) {
    struct distance_device_info *ddev = container_of(inode->i_cdev, struct distance_device_info, cdev);
    file->private_data = ddev;

    pr_devel("%s:distance meter open\n", __func__);
    return 0;
}

static int distance_close(struct inode *inode, struct file *file) {
    pr_devel("%s:beep closed\n", __func__);
    return 0;
}

// パルス幅はセンサー仕様でMAX38ms。従って、38000000/5400=7037mmを超えることはない。
#define result_char_size  6 // 4+1(\n)+1(NULL)

static ssize_t distance_read(struct file *fp, char __user *buf, size_t count, loff_t *f_pos) {
    struct distance_device_info *ddev = fp->private_data;
    unsigned int result_mm;
    char result_char[result_char_size];
    size_t result_length; 

    if (count == 0) return 0;
    if (buf == NULL) return -EINVAL;
    if (ddev == NULL) return -EBADF;

    result_mm = ddev->echo_length / 5686; //datasheetだと5400。実測で補正
    result_length = snprintf(result_char, result_char_size, "%d\n", result_mm);
    if (result_length > count) result_length=count;
    if (copy_to_user(buf, result_char, result_length)) {
        pr_devel("%s: 文字の転送に失敗した。\n",__func__);
        return -EFAULT;
    }
    
    pr_devel("%s: read(val=%d)\n", __func__, result_mm);
    return result_length;
}

        

/* "on"または"1"で始まる文字列なら測定を開始。
 * "off"または、"0"で始まる文字列なら、測定を停止。*/
void measure_start(struct timer_list *timer); 
static DEFINE_MUTEX(status_change); // タイマーの起動・停止作業中のロック

static ssize_t distance_write(struct file *fp, const char __user *buf, size_t count, loff_t *f_pos) {
    struct distance_device_info *ddev = fp->private_data;
    char write_str[4];
    size_t write_count;
    int result;
    char *i;

    pr_devel("%s: wrote.\n", __func__);

    write_count = (count>3) ? 3 : count;
    result = copy_from_user(write_str, buf, write_count);
    if (result != 0) {
        pr_devel("%s: 書き込まれた文字のコピーに失敗\n", __func__);
        return -EFAULT;
    }
    write_str[3]=0;

    for (i = write_str; *i!=0 ; i++) {
        *i = tolower(*i);
    }
    if ((write_str[0]=='o' && write_str[1]=='n') || write_str[0]=='1') {
        mutex_lock(&status_change);
        if ( !ddev->is_timer_on ) {
            ddev->is_timer_on = true;
            ddev->echo_start = 0;
            ddev->echo_length = 0;
            measure_start(&ddev->timer_for_measure);
            pr_devel("%s: タイマースタート\n", __func__);
        }
        mutex_unlock(&status_change);
    } else if ((write_str[0]=='o' && write_str[1]=='f' && write_str[2]=='f') || write_str[0]=='0' ){
        mutex_lock(&status_change);
        if ( ddev->is_timer_on ) {
            del_timer(&ddev->timer_for_measure);
            del_timer(&ddev->timer_for_triger_stop);
            gpiod_set_value(ddev->trig_gpio, 0);
            ddev->echo_start = 0;
            ddev->echo_length = 0;
            msleep(50); // 最終のトリガ停止後、次回スタートまで最低50ms必要。
            ddev->is_timer_on = false;
            pr_devel("%s: タイマー停止\n", __func__);
        }
        mutex_unlock(&status_change);
    }
    
    return count;
}

/* 定期的に測定を開始する　*/
void measure_start(struct timer_list *timer) {
    struct distance_device_info *ddev = container_of(timer, struct distance_device_info, timer_for_measure);
    
    gpiod_set_value(ddev->trig_gpio, 1);
    mod_timer(&ddev->timer_for_triger_stop, jiffies+1); // 1tickの待ちでも長過ぎるくらい。
    mod_timer(&ddev->timer_for_measure, jiffies+ddev->measure_span_jiffies); // タイマー再起動
}

/* トリガー信号停止(measure_startからのタイマー起動) */
void triger_signal_stop(struct timer_list *timer) {
    struct distance_device_info *ddev = container_of(timer, struct distance_device_info, timer_for_triger_stop);

    gpiod_set_value(ddev->trig_gpio, 0);
}
 
// echo信号の割込み処理。
// 立ち上がり時刻を保持し、立ち下がり時に信号の長さを計算する。
static irqreturn_t echo_irq_handler(int irq, void *device) {
    struct distance_device_info *ddev = (struct distance_device_info *)device;
    if (gpiod_get_value(ddev->echo_gpio) == 1) {
        ddev->echo_start = ktime_get();
    } else {
        ddev->echo_length = (int)(ktime_get() - ddev->echo_start);
    }
    return IRQ_HANDLED;
}

/* 仮に残しておく */
/*
static long beep_ioctl(struct file *fp, unsigned int cmd, unsigned long palm) {
    // cmd=1のみ有効。palmの値でringing_time_jiffiesを更新する。
    if (cmd == 1) {
        struct beep_device_info *bdev = fp->private_data;
        bdev->ringing_time_jiffies = msecs_to_jiffies((unsigned int)palm);
        pr_devel("%s: 鳴動時間変更(jiffies=%ld)(msec=%d)\n",
                __func__, bdev->ringing_time_jiffies, (unsigned int)palm); 
    }
    return 0;
}
*/

/* ハンドラ　テーブル */
struct file_operations distance_fops = {
    .open     = distance_open,
    .release  = distance_close,
    .read     = distance_read,
    .write    = distance_write,
    .unlocked_ioctl = NULL,
    .compat_ioctl = NULL,
};

// キャラクタデバイスの登録と、/dev/beep0の生成
static int make_udev(struct distance_device_info *ddev, const char* name) { 
    int ret = 0;
    dev_t dev;

    /* メジャー番号取得 */
    ret = alloc_chrdev_region(&dev, MINOR_BASE, MINOR_NUM, name);
    if (ret != 0) {
        pr_alert("%s: メジャー番号取得失敗(%d)\n", __func__, ret);
        goto err;
    }
    ddev->major = MAJOR(dev);

    /* カーネルへのキャラクタデバイスドライバ登録 */
    cdev_init(&ddev->cdev, &distance_fops);
    ddev->cdev.owner = THIS_MODULE;
    ret = cdev_add(&ddev->cdev, dev, MINOR_NUM);
    if (ret != 0) {
        pr_alert("%s: キャラクタデバイス登録失敗(%d)\n", __func__, ret);
        goto err_cdev_add;
    }

    /* カーネルクラス登録 */
    ddev->class = class_create(THIS_MODULE, name);
    if (IS_ERR(ddev->class)) {
        pr_alert("%s: カーネルクラス登録失敗\n", __func__);
        ret =  -PTR_ERR(ddev->class);
        goto err_class_create;
    }

    /* /sys/class/distance の生成 */
    device_create(ddev->class, NULL, MKDEV(ddev->major, 0), NULL, "distance");

    return 0;

err_class_create:
    cdev_del(&ddev->cdev);
err_cdev_add:
    unregister_chrdev_region(dev, MINOR_NUM);
err:
    return ret;
}

// キャラクタデバイス及び/dev/distanceの登録解除
static void remove_udev(struct distance_device_info *ddev) {
    dev_t dev = MKDEV(ddev->major, MINOR_BASE);
    device_destroy(ddev->class, MKDEV(ddev->major, 0));
    class_destroy(ddev->class); /* クラス登録解除 */
    cdev_del(&ddev->cdev); /* デバイス除去 */
    unregister_chrdev_region(dev, MINOR_NUM); /* メジャー番号除去 */
}

// sysfs ringing_timeの読み込みと書き込み
static ssize_t read_measure_span(struct device *dev, struct device_attribute *attr, char *buf) {
    struct distance_device_info *ddev = dev_get_drvdata(dev);
    int result;
    if (!ddev) {
        pr_err("%s: デバイス情報の取得に失敗しました。\n", __func__);
        return -EFAULT;
    }
    result = jiffies_to_msecs(ddev->measure_span_jiffies);
    return snprintf(buf, PAGE_SIZE, "%d\n", result);
}

#define MAX_LENGTH_LONG_NUM 11 // unsigned intの最大値　4,294,967,295
static ssize_t write_measure_span(struct device *dev, struct device_attribute *attr, const char *buf, size_t count) {
    struct distance_device_info *ddev;
    char source[MAX_LENGTH_LONG_NUM];
    unsigned int time_ms;
    int result;

    ddev = dev_get_drvdata(dev);
    if (!ddev) {
        pr_err("%s: デバイス情報の取得に失敗しました。\n", __func__);
        return -EFAULT;
    }

    if (count > MAX_LENGTH_LONG_NUM-1) {
        pr_err("%s: 引数が長過ぎる(length=%d)\n", __func__, count);
        return -EINVAL;
    }
    strncpy(source, buf, count);
    source[count] = 0;
    result = kstrtouint(source, 10, &time_ms);
    if (result) {
        pr_err("%s: 引数がおかしい(%s)\n",__func__, source);
        return -EINVAL;
    }
    if (time_ms < 50) time_ms=50;

    ddev->measure_span_jiffies = msecs_to_jiffies(time_ms);
    pr_devel("%s: 測定間隔をセットしました。(msec=%d)(jiffies=%ld)\n", 
            __func__, time_ms, ddev->measure_span_jiffies);
    return count;
}

// sysfs(/sys/device/platform/distance)の生成
static struct device_attribute dev_attr_measure_span = {
    .attr = {
        .name = "measure_span_ms",
        .mode = S_IRUGO | S_IWUSR,
    },
    .show = read_measure_span,
    .store = write_measure_span,
};

static int make_sysfs(struct device *dev) {
    return device_create_file(dev, &dev_attr_measure_span);
    return 0;
}

static void remove_sysfs(struct device *dev) {
    device_remove_file(dev, &dev_attr_measure_span);
}

// ドライバの初期化　及び　後始末
static const struct of_device_id of_distance_ids[] = {
    { .compatible = "crowpi2-distance" } ,
    { },
};

MODULE_DEVICE_TABLE(of, of_distance_ids);

static int distance_probe(struct platform_device *p_dev) {
    struct device *dev = &p_dev->dev;
    struct distance_device_info *ddev;
    int result;
    int echo_irq;

    if (!dev->of_node) {
        pr_alert("%s:Not Exist of_node for DISTANCE METER DRIVER. Check DTB\n", __func__);
        result = -ENODEV;
        goto err;
    }

    // デバイス情報のメモリ確保と初期化
    ddev = (struct distance_device_info*)devm_kzalloc(dev, sizeof(struct distance_device_info), GFP_KERNEL);
    if (!ddev) {
        pr_alert("%s: デバイス情報メモリ確保失敗\n", __func__);
        result = -ENOMEM;
        goto err;
    }
    dev_set_drvdata(dev, ddev);
    ddev->echo_start = 0;
    ddev->echo_length = 0;
    ddev->is_timer_on = false;

    // gpioの確保と初期化
    ddev->echo_gpio = devm_gpiod_get_index(dev, NULL, 0, GPIOD_IN);
    if (IS_ERR(ddev->echo_gpio)) {
        result = -PTR_ERR(ddev->echo_gpio);
        pr_alert("%s: can not get echo GPIO. ERR(%d)\n", __func__, result);
        goto err;
    }
    ddev->trig_gpio = devm_gpiod_get_index(dev, NULL, 1, GPIOD_OUT_LOW);
    if (IS_ERR(ddev->trig_gpio)) {
        result = -PTR_ERR(ddev->trig_gpio);
        pr_alert("%s: can not get triger GPIO. ERR(%d)\n", __func__, result);
        goto err_trig;
    }
    // echo信号用割込みの設定
    echo_irq = gpiod_to_irq(ddev->echo_gpio);
    if (echo_irq < 0) {
        result = echo_irq;
        pr_alert("%s: can not get IRQ for echo gpio. ERR(%d)\n", __func__, result);
        goto err_echo_irq;
    }
    result = request_irq(echo_irq, echo_irq_handler, IRQF_TRIGGER_RISING | IRQF_TRIGGER_FALLING, "distance_irq", ddev);
    if (result != 0) {
        pr_alert("%s: can not registration irq for echo gpio. ERR(%d)\n", __func__, result);
        goto err_echo_irq;
    }


    // udevの生成
    result = make_udev(ddev, p_dev->name);
    if (result != 0) {
        pr_alert("%s:Fail make udev. gpio desc dispose!!!\n", __func__);
        goto err_udev;
    }

    // sysfsの生成
    result = make_sysfs(dev);
    if (result != 0) {
        pr_alert("%s: sysfs生成失敗\n", __func__);
        goto err_sysfs;
    }
    
    // timerの生成
    timer_setup(&ddev->timer_for_measure, measure_start, 0);
    ddev->measure_span_jiffies= msecs_to_jiffies(100);
    timer_setup(&ddev->timer_for_triger_stop, triger_signal_stop, 0);

    pr_info("%s:distance meter driver init\n",__func__);
    return 0;

err_sysfs:
    remove_udev(ddev);
err_udev:
    free_irq(echo_irq, ddev);
err_echo_irq:
    gpiod_put(ddev->trig_gpio);
err_trig:
    gpiod_put(ddev->echo_gpio);
err:
    return result;
}

static int distance_remove(struct platform_device *p_dev) {
    struct distance_device_info *ddev = dev_get_drvdata(&p_dev->dev);
    int echo_irq;

    remove_udev(ddev);
    remove_sysfs(&p_dev->dev);

    // echo_gpio 割り込みハンドラの開放
    echo_irq = gpiod_to_irq(ddev->echo_gpio);
    free_irq(echo_irq, ddev);

    // gpioデバイスの開放
    if (ddev->echo_gpio) {
        gpiod_put(ddev->echo_gpio);
    }
    if (ddev->trig_gpio) {
        gpiod_set_value(ddev->trig_gpio, 0);
        gpiod_put(ddev->trig_gpio);
    }

    del_timer(&ddev->timer_for_measure);
    del_timer(&ddev->timer_for_triger_stop);

    pr_info("%s:distance meter driver unloaded\n",__func__);
    return 0;
} 
            

static struct platform_driver distance_driver = { 
    .probe = distance_probe,
    .remove = distance_remove,
    .driver = {
        .name = DRIVER_NAME,
        .owner = THIS_MODULE,
        .of_match_table = of_distance_ids,
    },
};

module_platform_driver(distance_driver);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("This is distance meter driver for crowpi2");
MODULE_AUTHOR("mito");
MODULE_VERSION("0.0.0");
