/*
 * SkyNET_CLK
 *
 * Copyright 2025 Benjamin Menkuec
 *
 */

#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/io.h>
#include <linux/dmaengine.h>
#include <linux/platform_device.h>
#include <linux/of.h>

#include <linux/iio/iio.h>
#include <linux/iio/sysfs.h>
#include <linux/iio/buffer.h>
#include <linux/iio/hw-consumer.h>

#include <linux/dma-direction.h>
#include <linux/iio/buffer_impl.h>
#include <linux/iio/buffer-dma.h>
#include <linux/iio/buffer-dmaengine.h>

#define AXI_REG_VERSION 0
#define MAX_CHANNEL 128

#define AXI_PCORE_VER(major, minor, patch)	\
	(((major) << 16) | ((minor) << 8) | (patch))
#define AXI_PCORE_VER_MAJOR(version)	(((version) >> 16) & 0xff)
#define AXI_PCORE_VER_MINOR(version)	(((version) >> 8) & 0xff)
#define AXI_PCORE_VER_PATCH(version)	((version) & 0xff)

struct sdr_chip_info {
	const struct iio_chan_spec *channels;
	unsigned int num_channels;
    unsigned int version;
};

struct axiadc_state {
	void __iomem			*regs;
	/* protect against device accesses */
	struct mutex			lock;
	unsigned int			max_usr_channel;
	struct iio_chan_spec		channels[MAX_CHANNEL];
    unsigned int            pcore_version;
};

static const struct sdr_chip_info skynet_clk_chip_info = {
	.channels = NULL,
	.num_channels = 0,
    .version = AXI_PCORE_VER(1, 0, 'a'),
};

static inline void axiadc_write(struct axiadc_state *st, unsigned reg, unsigned val)
{
	iowrite32(val, st->regs + reg);
}

static inline unsigned int axiadc_read(struct axiadc_state *st, unsigned reg)
{
	return ioread32(st->regs + reg);
}

static int sdr_reg_access(struct iio_dev *indio_dev,
			  unsigned int reg, unsigned int writeval,
			  unsigned int *readval)
{
	struct axiadc_state *st = iio_priv(indio_dev);

	mutex_lock(&st->lock);
    if (readval == NULL) {
        axiadc_write(st, reg & 0xFFFF, writeval);
    }
    else {
        *readval = axiadc_read(st, reg & 0xFFFF);
    }
	mutex_unlock(&st->lock);

	return 0;
}

static ssize_t show_reg(struct device *dev,
			   struct device_attribute *attr,
			   char *buf)
{
	struct iio_dev *indio_dev = dev_to_iio_dev(dev);
	struct axiadc_state *st = iio_priv(indio_dev);
	struct iio_dev_attr *this_attr = to_iio_dev_attr(attr);
    unsigned int readval = axiadc_read(st, (u32)this_attr->address);
	
    return sysfs_emit(buf, "%u\n", readval);
}

static ssize_t set_reg_int(struct device *dev,
			  struct device_attribute *attr,
			  const char *buf,
			  size_t len)
{
	struct iio_dev *indio_dev = dev_to_iio_dev(dev);
	struct axiadc_state *st = iio_priv(indio_dev);
	struct iio_dev_attr *this_attr = to_iio_dev_attr(attr);
    unsigned int writeval;
    int ret = kstrtouint(buf, 10, &writeval);
    if (ret)
		return ret;
    axiadc_write(st, (u32)this_attr->address, writeval);
	return len;
}

static IIO_DEVICE_ATTR(dac_voltage, S_IWUSR | S_IRUGO,
	show_reg, set_reg_int, 0x14);

static struct attribute *skynet_clk_attributes[] = {
	&iio_dev_attr_dac_voltage.dev_attr.attr,
	NULL,
};

static const struct attribute_group skynet_clk_group = {
	.attrs = skynet_clk_attributes,
};

static const struct iio_info sdr_info = {
	.read_raw = NULL,
	.write_raw = NULL,
	.debugfs_reg_access = &sdr_reg_access,
    .attrs = &skynet_clk_group,
	// .update_scan_mode = axiadc_update_scan_mode,
};

static const struct of_device_id sdr_of_match[] = {
	{ .compatible = "catkira,skynet_clk-1.00.a", .data = &skynet_clk_chip_info },
	{ /* end of list */ },
};
MODULE_DEVICE_TABLE(of, sdr_of_match);

static const struct iio_chan_spec dummy_channels[] = {
	{}
};

static int sdr_probe(struct platform_device *pdev)
{
	const struct sdr_chip_info *info;
	const struct of_device_id *id;
	struct iio_dev *indio_dev;
	struct axiadc_state *st;
	struct resource *mem;

	id = of_match_node(sdr_of_match, pdev->dev.of_node);
	if (!id)
		return -ENODEV;

	info = id->data;

	indio_dev = devm_iio_device_alloc(&pdev->dev, sizeof(*st));
	if (indio_dev == NULL)
		return -ENOMEM;

	st = iio_priv(indio_dev);
	mutex_init(&st->lock);

	mem = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	st->regs = devm_ioremap_resource(&pdev->dev, mem);
	if (IS_ERR(st->regs))
		return PTR_ERR(st->regs);

	platform_set_drvdata(pdev, indio_dev);

	indio_dev->dev.parent = &pdev->dev;
	indio_dev->name = pdev->dev.of_node->name;
	indio_dev->modes = INDIO_DIRECT_MODE;
	indio_dev->info = &sdr_info;

	/* Reset all HDL Cores */
	// axiadc_write(st, ADI_REG_RSTN, 0);
	// axiadc_write(st, ADI_REG_RSTN, ADI_RSTN);

    st->pcore_version = axiadc_read(st, AXI_REG_VERSION);
	if (AXI_PCORE_VER_MAJOR(st->pcore_version) >
		AXI_PCORE_VER_MAJOR(info->version)) {
		dev_err(&pdev->dev, "Major version mismatch between PCORE and driver. Driver expected %d.%.2d.%c, PCORE reported %d.%.2d.%c\n",
			AXI_PCORE_VER_MAJOR(info->version),
			AXI_PCORE_VER_MINOR(info->version),
			AXI_PCORE_VER_PATCH(info->version),
			AXI_PCORE_VER_MAJOR(st->pcore_version),
			AXI_PCORE_VER_MINOR(st->pcore_version),
			AXI_PCORE_VER_PATCH(st->pcore_version));
		return -ENODEV;
	}

    indio_dev->channels = info->channels;
    indio_dev->num_channels = info->num_channels;

	dev_info(&pdev->dev,
		 "Skynet CLK (%d.%.2d.%c) at 0x%08llX mapped to 0x%p\n",
		 AXI_PCORE_VER_MAJOR(st->pcore_version),
		 AXI_PCORE_VER_MINOR(st->pcore_version),
		 AXI_PCORE_VER_PATCH(st->pcore_version),
		 (unsigned long long)mem->start, st->regs);

	return devm_iio_device_register(&pdev->dev, indio_dev);
}

static struct platform_driver sdr_driver = {
	.driver = {
		.name = KBUILD_MODNAME,
		.of_match_table = sdr_of_match,
	},
	.probe	  = sdr_probe,
};

module_platform_driver(sdr_driver);

MODULE_AUTHOR("Benjamin Menkuec <benjamin@menkuec.de>");
MODULE_DESCRIPTION("SkyNET TX");
MODULE_LICENSE("closed source");
