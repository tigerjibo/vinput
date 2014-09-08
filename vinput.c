#define DEBUG
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/input.h>
#include <linux/spinlock.h>
#include <linux/slab.h>
#include <linux/types.h>
#include <linux/cdev.h>
#include <asm/uaccess.h>

#include "vinput.h"

#define DRIVER_NAME	"vinput"
#define VINPUT_MINORS	32

#define dev_to_vinput(dev)      container_of(dev, struct vinput, dev)

static DECLARE_BITMAP(vinput_ids, VINPUT_MINORS);

static LIST_HEAD(vinput_devices);
static LIST_HEAD(vinput_vdevices);

static dev_t vinput_dev;
static struct spinlock vinput_lock;
static struct class vinput_class;

struct vinput_device *vinput_get_device_by_type(const char *type)
{
	int found = 0;
	struct vinput_device *device;
	struct list_head *curr;

	spin_lock(&vinput_lock);
	list_for_each(curr, &vinput_devices) {
		device = list_entry(curr, struct vinput_device, list);
		if (strncmp(type, device->name, strlen(device->name)) == 0) {
			found = 1;
			break;
		}
	}
	spin_unlock(&vinput_lock);

	if (found)
		return device;
	return ERR_PTR(-ENODEV);
}

struct vinput *vinput_get_vdevice_by_id(long id)
{
	struct vinput *vinput = NULL;
	struct list_head *curr;

	spin_lock(&vinput_lock);
	list_for_each(curr, &vinput_vdevices) {
		vinput = list_entry(curr, struct vinput, list);
		if (vinput->id == id)
			break;
	}
	spin_unlock(&vinput_lock);

	if (vinput->id == id)
		return vinput;
	return ERR_PTR(-ENODEV);
}

static int vinput_open(struct inode *inode, struct file *file)
{
	int err = 0;
	struct vinput *vinput = NULL;

	vinput = vinput_get_vdevice_by_id(iminor(inode));

	if (IS_ERR(vinput))
		err = PTR_ERR(vinput);
	else
		file->private_data = vinput;

	return err;
}

static int vinput_release(struct inode *inode, struct file *file)
{
	return 0;
}

static ssize_t vinput_read(struct file *file, char __user *buffer,
			   size_t count, loff_t *offset)
{
	int len;
	char buff[VINPUT_MAX_LEN + 1];
	struct vinput *vinput = file->private_data;

	len = vinput->type->ops->read(vinput, buff, count);

	if (*offset > len)
		count = 0;
	else if (count + *offset > VINPUT_MAX_LEN)
		count = len - *offset;

	if (copy_to_user(buffer, buff + *offset, count))
		count = -EFAULT;

	*offset += count;

	return count;
}

static ssize_t vinput_write(struct file *file, const char __user *buffer,
			    size_t count, loff_t *offset)
{
	char buff[VINPUT_MAX_LEN + 1];
	struct vinput *vinput = file->private_data;

	memset(buff, 0, sizeof(char) * (VINPUT_MAX_LEN + 1));

	if (count > VINPUT_MAX_LEN) {
		dev_warn(&vinput->dev, "Too long. %d bytes allowed\n", VINPUT_MAX_LEN);
		return -EINVAL;
	}

	if (copy_from_user(buff, buffer, count))
		return -EFAULT;

	return vinput->type->ops->send(vinput, buff, count);
}

static const struct file_operations vinput_fops = {
	.owner = THIS_MODULE,
	.open = vinput_open,
	.release = vinput_release,
	.read = vinput_read,
	.write = vinput_write,
};

static void vinput_unregister_vdevice(struct vinput *vinput)
{
	input_unregister_device(vinput->input);
}

static void vinput_destroy_vdevice(struct vinput *vinput)
{
	/* Remove from the list first */
	spin_lock(&vinput_lock);
	list_del(&vinput->list);
	clear_bit(vinput->id, vinput_ids);
	spin_unlock(&vinput_lock);

	module_put(THIS_MODULE);

	kfree(vinput);
}

static void vinput_release_dev(struct device *dev)
{
	struct vinput *vinput = dev_to_vinput(dev);
	int id = vinput->id;

	vinput_destroy_vdevice(vinput);

	pr_debug("released vinput%d.\n", id);
}

static struct vinput *vinput_alloc_vdevice(void)
{
	int err;
	struct vinput *vinput = kzalloc(sizeof(struct vinput), GFP_KERNEL);

	try_module_get(THIS_MODULE);

	memset(vinput, 0, sizeof(struct vinput));

	spin_lock_init(&vinput->lock);

	spin_lock(&vinput_lock);
	vinput->id = find_first_zero_bit(vinput_ids, VINPUT_MINORS);
	if (vinput->id >= VINPUT_MINORS) {
		err = -ENOBUFS;
		goto fail_id;
	}
	set_bit(vinput->id, vinput_ids);
	list_add(&vinput->list, &vinput_vdevices);
	spin_unlock(&vinput_lock);

	/* allocate the input device */
	vinput->input = input_allocate_device();
	if (vinput->input == NULL) {
		pr_err("vinput: Cannot allocate vinput input device\n");
		err = -ENOMEM;
		goto fail_input_dev;
	}

	/* initialize device */
	vinput->dev.class = &vinput_class;
	vinput->dev.release = vinput_release_dev;
	vinput->dev.devt = MKDEV(vinput_dev, vinput->id);
	dev_set_name(&vinput->dev, DRIVER_NAME "%lu", vinput->id);

	return vinput;

fail_input_dev:
	spin_lock(&vinput_lock);
	list_del(&vinput->list);
fail_id:
	spin_unlock(&vinput_lock);
	module_put(THIS_MODULE);
	kfree(vinput);

	return ERR_PTR(err);
}

static int vinput_register_vdevice(struct vinput *vinput)
{
	int err = 0;

	/* register the input device */
	vinput->input->name = "vinput";
	vinput->input->phys = "vinput";
	vinput->input->dev.parent = &vinput->dev;

	vinput->input->id.bustype = BUS_VIRTUAL;
	vinput->input->id.product = 0x0000;
	vinput->input->id.vendor = 0x0000;
	vinput->input->id.version = 0x0000;

	err = vinput->type->ops->init(vinput);

	if (err == 0)
		dev_info(&vinput->dev, "Registered virtual input %s %ld\n",
			 vinput->type->name, vinput->id);

	return err;
}

static ssize_t export_store(struct class *class, struct class_attribute *attr,
			    const char *buf, size_t len)
{
	int err;
	struct vinput *vinput;
	struct vinput_device *device;

	device = vinput_get_device_by_type(buf);
	if (IS_ERR(device)) {
		pr_info("vinput: This virtual device isn't registered\n");
		err = PTR_ERR(device);
		goto fail;
	}

	vinput = vinput_alloc_vdevice();
	if (IS_ERR(vinput)) {
		err = PTR_ERR(vinput);
		goto fail;
	}

	vinput->type = device;
	err = vinput_register_vdevice(vinput);
	if (err < 0)
		goto fail_register_vinput;

	err = device_register(&vinput->dev);
	if (err < 0)
		goto fail_register;

	err = input_register_device(vinput->input);
	if (err < 0)
		goto fail_register_input;

	return len;

fail_register_input:
	device_unregister(&vinput->dev);
fail_register:
	vinput_unregister_vdevice(vinput);
fail_register_vinput:
	vinput_destroy_vdevice(vinput);
fail:
	return err;
}

static ssize_t unexport_store(struct class *class, struct class_attribute *attr,
			      const char *buf, size_t len)
{
	int err;
	unsigned long id;
	struct vinput *vinput;

	err = kstrtol(buf, 10, &id);
	if (err) {
		err = -EINVAL;
		goto failed;
	}

	vinput = vinput_get_vdevice_by_id(id);
	if (IS_ERR(vinput)) {
		pr_err("vinput: No such vinput device %ld\n", id);
		err = PTR_ERR(vinput);
		goto failed;
	}

	device_unregister(&vinput->dev);
	vinput_unregister_vdevice(vinput);

	return len;
failed:
	return err;
}

static struct class_attribute vinput_class_attrs[] = {
	__ATTR(export, 0200, NULL, export_store),
	__ATTR(unexport, 0200, NULL, unexport_store),
	__ATTR_NULL,
};

static struct class vinput_class = {
	.name = "vinput",
	.owner = THIS_MODULE,
	.class_attrs = vinput_class_attrs,
};

int vinput_register(struct vinput_device *dev)
{
	spin_lock(&vinput_lock);
	list_add(&dev->list, &vinput_devices);
	spin_unlock(&vinput_lock);

	pr_info("vinput: registered new virtual input device '%s'\n",
		dev->name);

	return 0;
}
EXPORT_SYMBOL(vinput_register);

void vinput_unregister(struct vinput_device *dev)
{
	struct list_head *curr, *next;

	/* Remove from the list first */
	spin_lock(&vinput_lock);
	list_del(&dev->list);
	spin_unlock(&vinput_lock);
	list_for_each_safe(curr, next, &vinput_vdevices) {
		struct vinput *vinput = list_entry(curr, struct vinput, list);
		device_unregister(&vinput->dev);
	}

	pr_info("vinput: unregistered virtual input device '%s'\n",
		dev->name);
}
EXPORT_SYMBOL(vinput_unregister);

static int __init vinput_init(void)
{
	int err = 0;

	pr_info("vinput: Loading virtual input driver\n");

	vinput_dev = register_chrdev(0, DRIVER_NAME, &vinput_fops);
	if (vinput_dev < 0) {
		pr_err("vinput: Unable to allocate char dev region\n");
		goto failed_alloc;
	}

	spin_lock_init(&vinput_lock);

	err = class_register(&vinput_class);
	if (err < 0) {
		pr_err("vinput: Unable to register vinput class\n");
		goto failed_class;
	}

	return 0;
failed_class:
	class_unregister(&vinput_class);
failed_alloc:
	return err;
}

static void __exit vinput_end(void)
{
	pr_info("vinput: Unloading virtual input driver\n");

	unregister_chrdev(vinput_dev, DRIVER_NAME);
	class_unregister(&vinput_class);
}

module_init(vinput_init);
module_exit(vinput_end);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Tristan Lelong <tristan.lelong@blunderer.org>");
MODULE_DESCRIPTION("emulate input events thru /dev/[vkbd | vts | vmouse]");
