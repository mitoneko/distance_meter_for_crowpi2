#define DEBUG 1
#include <linux/init.h>
#include <linux/module.h>
#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/sched.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/gpio/consumer.h>
#include <linux/timer.h>
#include <asm/current.h>
#include <asm/uaccess.h>

#define DRIVER_NAME "beep"

static const unsigned int MINOR_BASE = 0; // udev minor番号の始まり
static const unsigned int MINOR_NUM = 1;  // udev minorの個数
// デバイス全域で使用する変数達
// distance_probeでメモリ確保する。
struct distance_device_info {
    unsigned int major; // udev major番号
    struct cdev cdev;
    struct class *class;
    struct gpio_desc *gpio;
    struct timer_list ringing_timer;
    unsigned long ringing_time_jiffies; // 鳴動時間(単位:jiffies) 0で永遠
};

// /dev/distance配下のアクセス関数

// デバイス情報を取得し、file構造体に保存する。
static int distance_open(struct inode *inode, struct file *file) {
    pr_devel("%s:distance meter open\n", __func__);
    return 0;
}

static int distance_close(struct inode *inode, struct file *file) {
    pr_devel("%s:beep closed\n", __func__);
    return 0;
}

static ssize_t distance_read(struct file *fp, char __user *buf, size_t count, loff_t *f_pos) {
    pr_devl("%s: read\n", __func__);
    return 0;
}

static ssize_t distance_write(struct file *fp, const char __user *buf, size_t count, loff_t *f_pos) {
    pr_devl("%s: wrote.\n", __func__);
    return count;
}

/* 仮に残しておく */
void beep_off_when_timeup(struct timer_list *timer) {
    struct beep_device_info *ddev = container_of(timer, struct distance_device_info, ringing_timer);
    if (!ddev) {
        pr_err("%s:デバイス情報取得失敗\n", __func__);
        return;
    }
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
    cdev_init(&ddev->cdev, &beep_fops);
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
    dev_t dev = MKDEV(bdev->major, MINOR_BASE);
    device_destroy(bdev->class, MKDEV(bdev->major, 0));
    }
    class_destroy(bdev->class); /* クラス登録解除 */
    cdev_del(&ddev->cdev); /* デバイス除去 */
    unregister_chrdev_region(dev, MINOR_NUM); /* メジャー番号除去 */
}

// sysfs ringing_timeの読み込みと書き込み
/* 仮に残す
static ssize_t read_beep_ringing_time(struct device *dev, struct device_attribute *attr, char *buf) {
    struct distance_device_info *ddev = dev_get_drvdata(dev);
    if (!ddev) {
        pr_err("%s: デバイス情報の取得に失敗しました。\n", __func__);
        return -EFAULT;
    }
    return snprintf(buf, PAGE_SIZE, "%s\n", "kari");
}

#define MAX_LENGTH_LONG_NUM 11 // unsigned longの最大値　4,294,967,295
static ssize_t write_beep_ringing_time(struct device *dev, struct device_attribute *attr, const char *buf, size_t count) {
    struct beep_device_info *bdev;
    char source[MAX_LENGTH_LONG_NUM];
    unsigned int time_ms;
    int result;

    bdev = dev_get_drvdata(dev);
    if (!bdev) {
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

    bdev->ringing_time_jiffies = msecs_to_jiffies(time_ms);
    pr_devel("%s: ringing_timeをセットしました。(msec=%d)(jiffies=%ld)\n", 
            __func__, time_ms, bdev->ringing_time_jiffies);
    return count;
}
*/

// sysfs(/sys/device/platform/beep@0/beep_ringing_time)の生成
static struct device_attribute dev_attr_beep_ringing_time = {
    .attr = {
        .name = "hennsuuno_namae ni naru",
        .mode = S_IRUGO | S_IWUGO,
    },
    .show = NULL,
    .store = NULL,
};

static int make_sysfs(struct device *dev) {
    return device_create_file(dev, &dev_attr_beep_ringing_time);
}

static void remove_sysfs(struct device *dev) {
    device_remove_file(dev, &dev_attr_beep_ringing_time);
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

    if (!dev->of_node) {
        pr_alert("%s:Not Exist of_node for BEEP DRIVER. Check DTB\n", __func__);
        result = -ENODEV;
        goto err;
    }

    // デバイス情報のメモリ確保と初期化
    ddev = (struct beep_device_info*)devm_kzalloc(dev, sizeof(struct beep_device_info), GFP_KERNEL);
    if (!ddev) {
        pr_alert("%s: デバイス情報メモリ確保失敗\n", __func__);
        result = -ENOMEM;
        goto err;
    }
    dev_set_drvdata(dev, ddev);

    // gpioの確保と初期化
    // *************************************
    // *todo ２つ取れるように改造すること。*
    // *************************************
    ddev->gpio = devm_gpiod_get(dev, NULL, GPIOD_OUT_LOW);
    if (IS_ERR(ddev->gpio)) {
        result = -PTR_ERR(ddev->gpio);
        pr_alert("%s: can not get GPIO.ERR(%d)\n", __func__, result);
        goto err;
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
    // 多分作ることになる
    //timer_setup(&ddev->ringing_timer, beep_off_when_timeup, 0);
    //ddev->ringing_time_jiffies = msecs_to_jiffies(3000);

    pr_info("%s:distance meter driver init\n",__func__);
    return 0;

err_sysfs:
    remove_udev(ddev);
err_udev:
    gpiod_put(ddev->gpio);
err:
    return result;
}

static int distance_remove(struct platform_device *p_dev) {
    struct distance_device_info *ddev = dev_get_drvdata(&p_dev->dev);
    remove_udev(bdev);
    remove_sysfs(&p_dev->dev);

    // gpioデバイスの開放
    // ************************
    // 複数の開放が必要になる *
    // ************************
    if (ddev->gpio) {
        gpiod_put(ddev->gpio);
    }

    //多分、復活する.
    //del_timer(&ddev->ringing_timer);

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

