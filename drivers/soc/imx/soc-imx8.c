// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright 2019 NXP.
 */

#include <linux/arm-smccc.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/of_address.h>
#include <linux/slab.h>
#include <linux/sys_soc.h>
#include <linux/platform_device.h>
#include <linux/arm-smccc.h>
#include <linux/of.h>

#include <soc/imx/src.h>

#ifdef CONFIG_IWG27M
#include <linux/of_gpio.h>
#include <linux/gpio.h>
#include <linux/delay.h>
#endif

#define REV_B1				0x21

#define IMX8MQ_SW_INFO_B1		0x40
#define IMX8MQ_SW_MAGIC_B1		0xff0055aa

#define IMX_SIP_GET_SOC_INFO		0xc2000006

#define OCOTP_UID_LOW			0x410
#define OCOTP_UID_HIGH			0x420

/* Same as ANADIG_DIGPROG_IMX7D */
#define ANADIG_DIGPROG_IMX8MM	0x800

struct imx8_soc_data {
	char *name;
	u32 (*soc_revision)(void);
};

static u64 soc_uid;

static ssize_t soc_uid_show(struct device *dev,
			    struct device_attribute *attr, char *buf)
{
	return sprintf(buf, "%016llX\n", soc_uid);
}

static DEVICE_ATTR_RO(soc_uid);

static u32 imx8mq_soc_revision_from_atf(void)
{
	struct arm_smccc_res res;

	arm_smccc_smc(IMX_SIP_GET_SOC_INFO, 0, 0, 0, 0, 0, 0, 0, &res);

	if (res.a0 == SMCCC_RET_NOT_SUPPORTED)
		return 0;
	else
		return res.a0 & 0xff;
}

static u32 __init imx8mq_soc_revision(void)
{
	struct device_node *np;
	void __iomem *ocotp_base;
	u32 magic;
	u32 rev = 0;

	np = of_find_compatible_node(NULL, NULL, "fsl,imx8mq-ocotp");
	if (!np)
		goto out;

	ocotp_base = of_iomap(np, 0);
	WARN_ON(!ocotp_base);

	/*
	 * SOC revision on older imx8mq is not available in fuses so query
	 * the value from ATF instead.
	 */
	rev = imx8mq_soc_revision_from_atf();
	if (!rev) {
		magic = readl_relaxed(ocotp_base + IMX8MQ_SW_INFO_B1);
		if (magic == IMX8MQ_SW_MAGIC_B1)
			rev = REV_B1;
	}

	soc_uid = readl_relaxed(ocotp_base + OCOTP_UID_HIGH);
	soc_uid <<= 32;
	soc_uid |= readl_relaxed(ocotp_base + OCOTP_UID_LOW);

	iounmap(ocotp_base);

out:
	of_node_put(np);
	return rev;
}

static void __init imx8mm_soc_uid(void)
{
	void __iomem *ocotp_base;
	struct device_node *np;

	np = of_find_compatible_node(NULL, NULL, "fsl,imx8mm-ocotp");
	if (!np)
		return;

	ocotp_base = of_iomap(np, 0);
	WARN_ON(!ocotp_base);

	soc_uid = readl_relaxed(ocotp_base + OCOTP_UID_HIGH);
	soc_uid <<= 32;
	soc_uid |= readl_relaxed(ocotp_base + OCOTP_UID_LOW);

	iounmap(ocotp_base);
	of_node_put(np);
}

static u32 __init imx8mm_soc_revision(void)
{
	struct device_node *np;
	void __iomem *anatop_base;
	u32 rev;

	np = of_find_compatible_node(NULL, NULL, "fsl,imx8mm-anatop");
	if (!np)
		return 0;

	anatop_base = of_iomap(np, 0);
	WARN_ON(!anatop_base);

	rev = readl_relaxed(anatop_base + ANADIG_DIGPROG_IMX8MM);

	iounmap(anatop_base);
	of_node_put(np);

	imx8mm_soc_uid();

	return rev;
}

static const struct imx8_soc_data imx8mq_soc_data = {
	.name = "i.MX8MQ",
	.soc_revision = imx8mq_soc_revision,
};

static const struct imx8_soc_data imx8mm_soc_data = {
	.name = "i.MX8MM",
	.soc_revision = imx8mm_soc_revision,
};

static const struct imx8_soc_data imx8mn_soc_data = {
	.name = "i.MX8MN",
	.soc_revision = imx8mm_soc_revision,
};

static const struct of_device_id imx8_soc_match[] = {
	{ .compatible = "fsl,imx8mq", .data = &imx8mq_soc_data, },
	{ .compatible = "fsl,imx8mm", .data = &imx8mm_soc_data, },
	{ .compatible = "fsl,imx8mn", .data = &imx8mn_soc_data, },
	{ }
};

#define imx8_revision(soc_rev) \
	soc_rev ? \
	kasprintf(GFP_KERNEL, "%d.%d", (soc_rev >> 4) & 0xf,  soc_rev & 0xf) : \
	"unknown"

/* IWG27M: SOM Revision and BSP info */
#ifdef CONFIG_IWG27M

#define        BSP_VERSION             "iW-PRFHZ-SC-01-R4.0-REL1.0-Linux5.4.3"

static int __init som_revision(void)
{
	struct device_node *np;
	int i, val, err, pins_cnt;
	unsigned *pins;
	short revision = 0;

	np = of_find_node_by_path("/iwg27m_common");
	if (!np) {
		pr_warn("failed to find iwg27m-com node\n");
		revision =-1;
		goto put_node;
	}

	/* Fill GPIO pin array */
	pins_cnt = of_gpio_named_count(np, "som-rev-gpios");
	if (pins_cnt <= 0) {
		pr_warn("gpios DT property empty / missing\n");
		revision =-1;
		goto put_node;
	}

	pins = kzalloc(pins_cnt * sizeof(unsigned), GFP_KERNEL);
	if (!pins) {
		pr_warn("unable to allocate the memory\n");
		revision =-1;
		goto put_node;
	}
	for (i = 0; i < pins_cnt; i++) {

		val = of_get_named_gpio(np, "som-rev-gpios",i);
		if (val < 0) {
			pr_warn("unable to get the gpio\n");
			revision =-1;
			goto entryfail;
		}
		pins[i] = val;
	}

	/* Request as a input GPIO and read the value */
	for (i = 0; i < pins_cnt; i++) {
		err = gpio_request(pins[i],"som-rev GPIO");
		if (err){
			pr_warn("unable to request for gpio\n");
			revision =-1;
			goto entryfail;
		}

		err = gpio_direction_input(pins[i]);
		if (err) {
			pr_warn("unable to set gpio as input\n");
			revision =-1;
			goto entryfail;
		}

		revision |= gpio_get_value(pins[i]) << i;
		gpio_free(pins[i]);
	}

entryfail:
	kfree(pins);
put_node:
	of_node_put(np);
	return revision;
}

void print_board_info (void)
{
	int som_rev, pcb_rev, bom_rev;
	som_rev = som_revision();

	if (som_rev < 0) {
		pcb_rev = 0;
		bom_rev = 0;
	} else {
		pcb_rev = (((som_rev) & 0x30) >> 4) + 1 ;
		bom_rev = ((som_rev) & 0x0F) ;
	}

	printk ("\n");
	printk ("Board Info:\n");
	printk ("\tBSP Version     : %s\n", BSP_VERSION);
	printk ("\tSOM Version     : iW-PRFHZ-AP-01-R%x.%x\n", pcb_rev, bom_rev);
	printk ("\n");

}

void __init imx8_iwg27m_wifi(void)
{
        struct device_node *np;
        static int wifi_dev_gpio, wifi_host_gpio;
        static int bt_dev_gpio, bt_host_gpio;
        static int wl_en_gpio, bt_en_gpio;

        np = of_find_node_by_path("/bus@5b000000/mmc@5b030000");
        if (!np) {
                printk("\n IWG failed to find SDHC3 node\n");
                goto put_node;
        }

        wl_en_gpio = of_get_named_gpio(np, "wl-en-gpio", 0);
        if (gpio_is_valid(wl_en_gpio) &&
                        !gpio_request_one(wl_en_gpio, GPIOF_OUT_INIT_HIGH, "wl-en")) {

                gpio_set_value(wl_en_gpio, 1);
        }

        bt_en_gpio = of_get_named_gpio(np, "bt-en-gpio", 0);
        if (gpio_is_valid(bt_en_gpio) &&
                        !gpio_request_one(bt_en_gpio, GPIOF_OUT_INIT_HIGH, "bt-en")) {

                gpio_set_value(bt_en_gpio, 1);
        }

        wifi_host_gpio = of_get_named_gpio(np, "wifi-host-gpio", 0);
        if (gpio_is_valid(wifi_host_gpio) &&
                        !gpio_request_one(wifi_host_gpio, GPIOF_IN, "wifi_host")) {

                gpio_direction_input(wifi_host_gpio);
        }

        wifi_dev_gpio = of_get_named_gpio(np, "wifi-dev-gpio", 0);

        if (gpio_is_valid(wifi_dev_gpio) &&
                        !gpio_request_one(wifi_dev_gpio, GPIOF_OUT_INIT_HIGH, "wifi_dev")) {

                gpio_set_value(wifi_dev_gpio,1);
        }

        bt_host_gpio = of_get_named_gpio(np, "bt-host-gpio", 0);
        if (gpio_is_valid(bt_host_gpio) &&
                        !gpio_request_one(bt_host_gpio, GPIOF_IN, "bt_host")) {

                gpio_direction_input(bt_host_gpio);
        }

        bt_dev_gpio = of_get_named_gpio(np, "bt-dev-gpio", 0);

        if (gpio_is_valid(bt_dev_gpio) &&
                        !gpio_request_one(bt_dev_gpio, GPIOF_OUT_INIT_HIGH, "bt_dev")) {

                gpio_set_value(bt_dev_gpio,1);
        }

put_node:
        of_node_put(np);
}
#endif

static int __init imx8_soc_init(void)
{
	struct soc_device_attribute *soc_dev_attr;
	struct soc_device *soc_dev;
	const struct of_device_id *id;
	u32 soc_rev = 0;
	const struct imx8_soc_data *data;
	int ret;

	soc_dev_attr = kzalloc(sizeof(*soc_dev_attr), GFP_KERNEL);
	if (!soc_dev_attr)
		return -ENOMEM;

#ifdef CONFIG_IWG27M
        /* IWG27M: Board and BSP info Print */
        print_board_info();
        /* IWG27M: JODY-W2 WIFI-BT Power Sequence */
	imx8_iwg27m_wifi();
#endif
	soc_dev_attr->family = "Freescale i.MX";

	ret = of_property_read_string(of_root, "model", &soc_dev_attr->machine);
	if (ret)
		goto free_soc;

	id = of_match_node(imx8_soc_match, of_root);
	if (!id) {
		ret = -ENODEV;
		goto free_soc;
	}

	data = id->data;
	if (data) {
		soc_dev_attr->soc_id = data->name;
		if (data->soc_revision)
			soc_rev = data->soc_revision();
	}

	soc_dev_attr->revision = imx8_revision(soc_rev);
	if (!soc_dev_attr->revision) {
		ret = -ENOMEM;
		goto free_soc;
	}

	soc_dev = soc_device_register(soc_dev_attr);
	if (IS_ERR(soc_dev)) {
		ret = PTR_ERR(soc_dev);
		goto free_rev;
	}

	ret = device_create_file(soc_device_to_device(soc_dev),
				 &dev_attr_soc_uid);
	if (ret)
		goto free_rev;

	if (IS_ENABLED(CONFIG_ARM_IMX_CPUFREQ_DT))
		platform_device_register_simple("imx-cpufreq-dt", -1, NULL, 0);

	return 0;

free_rev:
	if (strcmp(soc_dev_attr->revision, "unknown"))
		kfree(soc_dev_attr->revision);
free_soc:
	kfree(soc_dev_attr);
	return ret;
}
device_initcall(imx8_soc_init);

#define FSL_SIP_SRC                    0xc2000005
#define FSL_SIP_SRC_M4_START           0x00
#define FSL_SIP_SRC_M4_STARTED         0x01

/* To indicate M4 enabled or not on i.MX8MQ */
static bool m4_is_enabled;
bool imx_src_is_m4_enabled(void)
{
	return m4_is_enabled;
}

int check_m4_enabled(void)
{
	struct arm_smccc_res res;

	arm_smccc_smc(FSL_SIP_SRC, FSL_SIP_SRC_M4_STARTED, 0,
		      0, 0, 0, 0, 0, &res);
		      m4_is_enabled = !!res.a0;

	if (m4_is_enabled)
		printk("M4 is started\n");

	return 0;
}
