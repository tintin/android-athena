/* ds.c: Domain Services driver for Logical Domains
 *
 * Copyright (C) 2007 David S. Miller <davem@davemloft.net>
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/types.h>
#include <linux/module.h>
#include <linux/string.h>
#include <linux/slab.h>
#include <linux/sched.h>
#include <linux/delay.h>
#include <linux/mutex.h>
#include <linux/kthread.h>
#include <linux/cpu.h>

#include <asm/ldc.h>
#include <asm/vio.h>
#include <asm/power.h>
#include <asm/mdesc.h>
#include <asm/head.h>
#include <asm/irq.h>

#define DRV_MODULE_NAME		"ds"
#define PFX DRV_MODULE_NAME	": "
#define DRV_MODULE_VERSION	"1.0"
#define DRV_MODULE_RELDATE	"Jul 11, 2007"

static char version[] __devinitdata =
	DRV_MODULE_NAME ".c:v" DRV_MODULE_VERSION " (" DRV_MODULE_RELDATE ")\n";
MODULE_AUTHOR("David S. Miller (davem@davemloft.net)");
MODULE_DESCRIPTION("Sun LDOM domain services driver");
MODULE_LICENSE("GPL");
MODULE_VERSION(DRV_MODULE_VERSION);

struct ds_msg_tag {
	__u32			type;
#define DS_INIT_REQ		0x00
#define DS_INIT_ACK		0x01
#define DS_INIT_NACK		0x02
#define DS_REG_REQ		0x03
#define DS_REG_ACK		0x04
#define DS_REG_NACK		0x05
#define DS_UNREG_REQ		0x06
#define DS_UNREG_ACK		0x07
#define DS_UNREG_NACK		0x08
#define DS_DATA			0x09
#define DS_NACK			0x0a

	__u32			len;
};

/* Result codes */
#define DS_OK			0x00
#define DS_REG_VER_NACK		0x01
#define DS_REG_DUP		0x02
#define DS_INV_HDL		0x03
#define DS_TYPE_UNKNOWN		0x04

struct ds_version {
	__u16			major;
	__u16			minor;
};

struct ds_ver_req {
	struct ds_msg_tag	tag;
	struct ds_version	ver;
};

struct ds_ver_ack {
	struct ds_msg_tag	tag;
	__u16			minor;
};

struct ds_ver_nack {
	struct ds_msg_tag	tag;
	__u16			major;
};

struct ds_reg_req {
	struct ds_msg_tag	tag;
	__u64			handle;
	__u16			major;
	__u16			minor;
	char			svc_id[0];
};

struct ds_reg_ack {
	struct ds_msg_tag	tag;
	__u64			handle;
	__u16			minor;
};

struct ds_reg_nack {
	struct ds_msg_tag	tag;
	__u64			handle;
	__u16			major;
};

struct ds_unreg_req {
	struct ds_msg_tag	tag;
	__u64			handle;
};

struct ds_unreg_ack {
	struct ds_msg_tag	tag;
	__u64			handle;
};

struct ds_unreg_nack {
	struct ds_msg_tag	tag;
	__u64			handle;
};

struct ds_data {
	struct ds_msg_tag	tag;
	__u64			handle;
};

struct ds_data_nack {
	struct ds_msg_tag	tag;
	__u64			handle;
	__u64			result;
};

struct ds_cap_state {
	__u64			handle;

	void			(*data)(struct ldc_channel *lp,
					struct ds_cap_state *cp,
					void *buf, int len);

	const char		*service_id;

	u8			state;
#define CAP_STATE_UNKNOWN	0x00
#define CAP_STATE_REG_SENT	0x01
#define CAP_STATE_REGISTERED	0x02
};

static void md_update_data(struct ldc_channel *lp, struct ds_cap_state *cp,
			   void *buf, int len);
static void domain_shutdown_data(struct ldc_channel *lp,
				 struct ds_cap_state *cp,
				 void *buf, int len);
static void domain_panic_data(struct ldc_channel *lp,
			      struct ds_cap_state *cp,
			      void *buf, int len);
#ifdef CONFIG_HOTPLUG_CPU
static void dr_cpu_data(struct ldc_channel *lp,
			struct ds_cap_state *cp,
			void *buf, int len);
#endif
static void ds_pri_data(struct ldc_channel *lp,
			struct ds_cap_state *cp,
			void *buf, int len);
static void ds_var_data(struct ldc_channel *lp,
			struct ds_cap_state *cp,
			void *buf, int len);

struct ds_cap_state ds_states[] = {
	{
		.service_id	= "md-update",
		.data		= md_update_data,
	},
	{
		.service_id	= "domain-shutdown",
		.data		= domain_shutdown_data,
	},
	{
		.service_id	= "domain-panic",
		.data		= domain_panic_data,
	},
#ifdef CONFIG_HOTPLUG_CPU
	{
		.service_id	= "dr-cpu",
		.data		= dr_cpu_data,
	},
#endif
	{
		.service_id	= "pri",
		.data		= ds_pri_data,
	},
	{
		.service_id	= "var-config",
		.data		= ds_var_data,
	},
	{
		.service_id	= "var-config-backup",
		.data		= ds_var_data,
	},
};

static DEFINE_SPINLOCK(ds_lock);

struct ds_info {
	struct ldc_channel	*lp;
	u8			hs_state;
#define DS_HS_START		0x01
#define DS_HS_DONE		0x02

	void			*rcv_buf;
	int			rcv_buf_len;
};

static struct ds_info *ds_info;

static struct ds_cap_state *find_cap(u64 handle)
{
	unsigned int index = handle >> 32;

	if (index >= ARRAY_SIZE(ds_states))
		return NULL;
	return &ds_states[index];
}

static struct ds_cap_state *find_cap_by_string(const char *name)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(ds_states); i++) {
		if (strcmp(ds_states[i].service_id, name))
			continue;

		return &ds_states[i];
	}
	return NULL;
}

static int ds_send(struct ldc_channel *lp, void *data, int len)
{
	int err, limit = 1000;

	err = -EINVAL;
	while (limit-- > 0) {
		err = ldc_write(lp, data, len);
		if (!err || (err != -EAGAIN))
			break;
		udelay(1);
	}

	return err;
}

struct ds_md_update_req {
	__u64				req_num;
};

struct ds_md_update_res {
	__u64				req_num;
	__u32				result;
};

static void md_update_data(struct ldc_channel *lp,
			   struct ds_cap_state *dp,
			   void *buf, int len)
{
	struct ds_data *dpkt = buf;
	struct ds_md_update_req *rp;
	struct {
		struct ds_data		data;
		struct ds_md_update_res	res;
	} pkt;

	rp = (struct ds_md_update_req *) (dpkt + 1);

	printk(KERN_INFO PFX "Machine description update.\n");

	memset(&pkt, 0, sizeof(pkt));
	pkt.data.tag.type = DS_DATA;
	pkt.data.tag.len = sizeof(pkt) - sizeof(struct ds_msg_tag);
	pkt.data.handle = dp->handle;
	pkt.res.req_num = rp->req_num;
	pkt.res.result = DS_OK;

	ds_send(lp, &pkt, sizeof(pkt));

	mdesc_update();
}

struct ds_shutdown_req {
	__u64				req_num;
	__u32				ms_delay;
};

struct ds_shutdown_res {
	__u64				req_num;
	__u32				result;
	char				reason[1];
};

static void domain_shutdown_data(struct ldc_channel *lp,
				 struct ds_cap_state *dp,
				 void *buf, int len)
{
	struct ds_data *dpkt = buf;
	struct ds_shutdown_req *rp;
	struct {
		struct ds_data		data;
		struct ds_shutdown_res	res;
	} pkt;

	rp = (struct ds_shutdown_req *) (dpkt + 1);

	printk(KERN_ALERT PFX "Shutdown request from "
	       "LDOM manager received.\n");

	memset(&pkt, 0, sizeof(pkt));
	pkt.data.tag.type = DS_DATA;
	pkt.data.tag.len = sizeof(pkt) - sizeof(struct ds_msg_tag);
	pkt.data.handle = dp->handle;
	pkt.res.req_num = rp->req_num;
	pkt.res.result = DS_OK;
	pkt.res.reason[0] = 0;

	ds_send(lp, &pkt, sizeof(pkt));

	wake_up_powerd();
}

struct ds_panic_req {
	__u64				req_num;
};

struct ds_panic_res {
	__u64				req_num;
	__u32				result;
	char				reason[1];
};

static void domain_panic_data(struct ldc_channel *lp,
			      struct ds_cap_state *dp,
			      void *buf, int len)
{
	struct ds_data *dpkt = buf;
	struct ds_panic_req *rp;
	struct {
		struct ds_data		data;
		struct ds_panic_res	res;
	} pkt;

	rp = (struct ds_panic_req *) (dpkt + 1);

	printk(KERN_ALERT PFX "Panic request from "
	       "LDOM manager received.\n");

	memset(&pkt, 0, sizeof(pkt));
	pkt.data.tag.type = DS_DATA;
	pkt.data.tag.len = sizeof(pkt) - sizeof(struct ds_msg_tag);
	pkt.data.handle = dp->handle;
	pkt.res.req_num = rp->req_num;
	pkt.res.result = DS_OK;
	pkt.res.reason[0] = 0;

	ds_send(lp, &pkt, sizeof(pkt));

	panic("PANIC requested by LDOM manager.");
}

#ifdef CONFIG_HOTPLUG_CPU
struct dr_cpu_tag {
	__u64				req_num;
	__u32				type;
#define DR_CPU_CONFIGURE		0x43
#define DR_CPU_UNCONFIGURE		0x55
#define DR_CPU_FORCE_UNCONFIGURE	0x46
#define DR_CPU_STATUS			0x53

/* Responses */
#define DR_CPU_OK			0x6f
#define DR_CPU_ERROR			0x65

	__u32				num_records;
};

struct dr_cpu_resp_entry {
	__u32				cpu;
	__u32				result;
#define DR_CPU_RES_OK			0x00
#define DR_CPU_RES_FAILURE		0x01
#define DR_CPU_RES_BLOCKED		0x02
#define DR_CPU_RES_CPU_NOT_RESPONDING	0x03
#define DR_CPU_RES_NOT_IN_MD		0x04

	__u32				stat;
#define DR_CPU_STAT_NOT_PRESENT		0x00
#define DR_CPU_STAT_UNCONFIGURED	0x01
#define DR_CPU_STAT_CONFIGURED		0x02

	__u32				str_off;
};

/* DR cpu requests get queued onto the work list by the
 * dr_cpu_data() callback.  The list is protected by
 * ds_lock, and processed by dr_cpu_process() in order.
 */
static LIST_HEAD(dr_cpu_work_list);
static DECLARE_WAIT_QUEUE_HEAD(dr_cpu_wait);

struct dr_cpu_queue_entry {
	struct list_head		list;
	char				req[0];
};

static void __dr_cpu_send_error(struct ds_cap_state *cp, struct ds_data *data)
{
	struct dr_cpu_tag *tag = (struct dr_cpu_tag *) (data + 1);
	struct ds_info *dp = ds_info;
	struct {
		struct ds_data		data;
		struct dr_cpu_tag	tag;
	} pkt;
	int msg_len;

	memset(&pkt, 0, sizeof(pkt));
	pkt.data.tag.type = DS_DATA;
	pkt.data.handle = cp->handle;
	pkt.tag.req_num = tag->req_num;
	pkt.tag.type = DR_CPU_ERROR;
	pkt.tag.num_records = 0;

	msg_len = (sizeof(struct ds_data) +
		   sizeof(struct dr_cpu_tag));

	pkt.data.tag.len = msg_len - sizeof(struct ds_msg_tag);

	ds_send(dp->lp, &pkt, msg_len);
}

static void dr_cpu_send_error(struct ds_cap_state *cp, struct ds_data *data)
{
	unsigned long flags;

	spin_lock_irqsave(&ds_lock, flags);
	__dr_cpu_send_error(cp, data);
	spin_unlock_irqrestore(&ds_lock, flags);
}

#define CPU_SENTINEL	0xffffffff

static void purge_dups(u32 *list, u32 num_ents)
{
	unsigned int i;

	for (i = 0; i < num_ents; i++) {
		u32 cpu = list[i];
		unsigned int j;

		if (cpu == CPU_SENTINEL)
			continue;

		for (j = i + 1; j < num_ents; j++) {
			if (list[j] == cpu)
				list[j] = CPU_SENTINEL;
		}
	}
}

static int dr_cpu_size_response(int ncpus)
{
	return (sizeof(struct ds_data) +
		sizeof(struct dr_cpu_tag) +
		(sizeof(struct dr_cpu_resp_entry) * ncpus));
}

static void dr_cpu_init_response(struct ds_data *resp, u64 req_num,
				 u64 handle, int resp_len, int ncpus,
				 cpumask_t *mask, u32 default_stat)
{
	struct dr_cpu_resp_entry *ent;
	struct dr_cpu_tag *tag;
	int i, cpu;

	tag = (struct dr_cpu_tag *) (resp + 1);
	ent = (struct dr_cpu_resp_entry *) (tag + 1);

	resp->tag.type = DS_DATA;
	resp->tag.len = resp_len - sizeof(struct ds_msg_tag);
	resp->handle = handle;
	tag->req_num = req_num;
	tag->type = DR_CPU_OK;
	tag->num_records = ncpus;

	i = 0;
	for_each_cpu_mask(cpu, *mask) {
		ent[i].cpu = cpu;
		ent[i].result = DR_CPU_RES_OK;
		ent[i].stat = default_stat;
		i++;
	}
	BUG_ON(i != ncpus);
}

static void dr_cpu_mark(struct ds_data *resp, int cpu, int ncpus,
			u32 res, u32 stat)
{
	struct dr_cpu_resp_entry *ent;
	struct dr_cpu_tag *tag;
	int i;

	tag = (struct dr_cpu_tag *) (resp + 1);
	ent = (struct dr_cpu_resp_entry *) (tag + 1);

	for (i = 0; i < ncpus; i++) {
		if (ent[i].cpu != cpu)
			continue;
		ent[i].result = res;
		ent[i].stat = stat;
		break;
	}
}

static int dr_cpu_configure(struct ds_cap_state *cp, u64 req_num,
			    cpumask_t *mask)
{
	struct ds_data *resp;
	int resp_len, ncpus, cpu;
	unsigned long flags;

	ncpus = cpus_weight(*mask);
	resp_len = dr_cpu_size_response(ncpus);
	resp = kzalloc(resp_len, GFP_KERNEL);
	if (!resp)
		return -ENOMEM;

	dr_cpu_init_response(resp, req_num, cp->handle,
			     resp_len, ncpus, mask,
			     DR_CPU_STAT_CONFIGURED);

	mdesc_fill_in_cpu_data(*mask);

	for_each_cpu_mask(cpu, *mask) {
		int err;

		printk(KERN_INFO PFX "Starting cpu %d...\n", cpu);
		err = cpu_up(cpu);
		if (err) {
			__u32 res = DR_CPU_RES_FAILURE;
			__u32 stat = DR_CPU_STAT_UNCONFIGURED;

			if (!cpu_present(cpu)) {
				/* CPU not present in MD */
				res = DR_CPU_RES_NOT_IN_MD;
				stat = DR_CPU_STAT_NOT_PRESENT;
			} else if (err == -ENODEV) {
				/* CPU did not call in successfully */
				res = DR_CPU_RES_CPU_NOT_RESPONDING;
			}

			printk(KERN_INFO PFX "CPU startup failed err=%d\n",
			       err);
			dr_cpu_mark(resp, cpu, ncpus, res, stat);
		}
	}

	spin_lock_irqsave(&ds_lock, flags);
	ds_send(ds_info->lp, resp, resp_len);
	spin_unlock_irqrestore(&ds_lock, flags);

	kfree(resp);

	/* Redistribute IRQs, taking into account the new cpus.  */
	fixup_irqs();

	return 0;
}

static int dr_cpu_unconfigure(struct ds_cap_state *cp, u64 req_num,
			      cpumask_t *mask)
{
	struct ds_data *resp;
	int resp_len, ncpus, cpu;
	unsigned long flags;

	ncpus = cpus_weight(*mask);
	resp_len = dr_cpu_size_response(ncpus);
	resp = kzalloc(resp_len, GFP_KERNEL);
	if (!resp)
		return -ENOMEM;

	dr_cpu_init_response(resp, req_num, cp->handle,
			     resp_len, ncpus, mask,
			     DR_CPU_STAT_UNCONFIGURED);

	for_each_cpu_mask(cpu, *mask) {
		int err;

		printk(KERN_INFO PFX "CPU[%d]: Shutting down cpu %d...\n",
		       smp_processor_id(), cpu);
		err = cpu_down(cpu);
		if (err)
			dr_cpu_mark(resp, cpu, ncpus,
				    DR_CPU_RES_FAILURE,
				    DR_CPU_STAT_CONFIGURED);
	}

	spin_lock_irqsave(&ds_lock, flags);
	ds_send(ds_info->lp, resp, resp_len);
	spin_unlock_irqrestore(&ds_lock, flags);

	kfree(resp);

	return 0;
}

static void process_dr_cpu_list(struct ds_cap_state *cp)
{
	struct dr_cpu_queue_entry *qp, *tmp;
	unsigned long flags;
	LIST_HEAD(todo);
	cpumask_t mask;

	spin_lock_irqsave(&ds_lock, flags);
	list_splice(&dr_cpu_work_list, &todo);
	INIT_LIST_HEAD(&dr_cpu_work_list);
	spin_unlock_irqrestore(&ds_lock, flags);

	list_for_each_entry_safe(qp, tmp, &todo, list) {
		struct ds_data *data = (struct ds_data *) qp->req;
		struct dr_cpu_tag *tag = (struct dr_cpu_tag *) (data + 1);
		u32 *cpu_list = (u32 *) (tag + 1);
		u64 req_num = tag->req_num;
		unsigned int i;
		int err;

		switch (tag->type) {
		case DR_CPU_CONFIGURE:
		case DR_CPU_UNCONFIGURE:
		case DR_CPU_FORCE_UNCONFIGURE:
			break;

		default:
			dr_cpu_send_error(cp, data);
			goto next;
		}

		purge_dups(cpu_list, tag->num_records);

		cpus_clear(mask);
		for (i = 0; i < tag->num_records; i++) {
			if (cpu_list[i] == CPU_SENTINEL)
				continue;

			if (cpu_list[i] < NR_CPUS)
				cpu_set(cpu_list[i], mask);
		}

		if (tag->type == DR_CPU_CONFIGURE)
			err = dr_cpu_configure(cp, req_num, &mask);
		else
			err = dr_cpu_unconfigure(cp, req_num, &mask);

		if (err)
			dr_cpu_send_error(cp, data);

next:
		list_del(&qp->list);
		kfree(qp);
	}
}

static int dr_cpu_thread(void *__unused)
{
	struct ds_cap_state *cp;
	DEFINE_WAIT(wait);

	cp = find_cap_by_string("dr-cpu");

	while (1) {
		prepare_to_wait(&dr_cpu_wait, &wait, TASK_INTERRUPTIBLE);
		if (list_empty(&dr_cpu_work_list))
			schedule();
		finish_wait(&dr_cpu_wait, &wait);

		if (kthread_should_stop())
			break;

		process_dr_cpu_list(cp);
	}

	return 0;
}

static void dr_cpu_data(struct ldc_channel *lp,
			struct ds_cap_state *dp,
			void *buf, int len)
{
	struct dr_cpu_queue_entry *qp;
	struct ds_data *dpkt = buf;
	struct dr_cpu_tag *rp;

	rp = (struct dr_cpu_tag *) (dpkt + 1);

	qp = kmalloc(sizeof(struct dr_cpu_queue_entry) + len, GFP_ATOMIC);
	if (!qp) {
		struct ds_cap_state *cp;

		cp = find_cap_by_string("dr-cpu");
		__dr_cpu_send_error(cp, dpkt);
	} else {
		memcpy(&qp->req, buf, len);
		list_add_tail(&qp->list, &dr_cpu_work_list);
		wake_up(&dr_cpu_wait);
	}
}
#endif

struct ds_pri_msg {
	__u64				req_num;
	__u64				type;
#define DS_PRI_REQUEST			0x00
#define DS_PRI_DATA			0x01
#define DS_PRI_UPDATE			0x02
};

static void ds_pri_data(struct ldc_channel *lp,
			struct ds_cap_state *dp,
			void *buf, int len)
{
	struct ds_data *dpkt = buf;
	struct ds_pri_msg *rp;

	rp = (struct ds_pri_msg *) (dpkt + 1);

	printk(KERN_INFO PFX "PRI REQ [%lx:%lx], len=%d\n",
	       rp->req_num, rp->type, len);
}

struct ds_var_hdr {
	__u32				type;
#define DS_VAR_SET_REQ			0x00
#define DS_VAR_DELETE_REQ		0x01
#define DS_VAR_SET_RESP			0x02
#define DS_VAR_DELETE_RESP		0x03
};

struct ds_var_set_msg {
	struct ds_var_hdr		hdr;
	char				name_and_value[0];
};

struct ds_var_delete_msg {
	struct ds_var_hdr		hdr;
	char				name[0];
};

struct ds_var_resp {
	struct ds_var_hdr		hdr;
	__u32				result;
#define DS_VAR_SUCCESS			0x00
#define DS_VAR_NO_SPACE			0x01
#define DS_VAR_INVALID_VAR		0x02
#define DS_VAR_INVALID_VAL		0x03
#define DS_VAR_NOT_PRESENT		0x04
};

static DEFINE_MUTEX(ds_var_mutex);
static int ds_var_doorbell;
static int ds_var_response;

static void ds_var_data(struct ldc_channel *lp,
			struct ds_cap_state *dp,
			void *buf, int len)
{
	struct ds_data *dpkt = buf;
	struct ds_var_resp *rp;

	rp = (struct ds_var_resp *) (dpkt + 1);

	if (rp->hdr.type != DS_VAR_SET_RESP &&
	    rp->hdr.type != DS_VAR_DELETE_RESP)
		return;

	ds_var_response = rp->result;
	wmb();
	ds_var_doorbell = 1;
}

void ldom_set_var(const char *var, const char *value)
{
	struct ds_info *dp = ds_info;
	struct ds_cap_state *cp;

	cp = find_cap_by_string("var-config");
	if (cp->state != CAP_STATE_REGISTERED)
		cp = find_cap_by_string("var-config-backup");

	if (cp->state == CAP_STATE_REGISTERED) {
		union {
			struct {
				struct ds_data		data;
				struct ds_var_set_msg	msg;
			} header;
			char			all[512];
		} pkt;
		unsigned long flags;
		char  *base, *p;
		int msg_len, loops;

		memset(&pkt, 0, sizeof(pkt));
		pkt.header.data.tag.type = DS_DATA;
		pkt.header.data.handle = cp->handle;
		pkt.header.msg.hdr.type = DS_VAR_SET_REQ;
		base = p = &pkt.header.msg.name_and_value[0];
		strcpy(p, var);
		p += strlen(var) + 1;
		strcpy(p, value);
		p += strlen(value) + 1;

		msg_len = (sizeof(struct ds_data) +
			   sizeof(struct ds_var_set_msg) +
			   (p - base));
		msg_len = (msg_len + 3) & ~3;
		pkt.header.data.tag.len = msg_len - sizeof(struct ds_msg_tag);

		mutex_lock(&ds_var_mutex);

		spin_lock_irqsave(&ds_lock, flags);
		ds_var_doorbell = 0;
		ds_var_response = -1;

		ds_send(dp->lp, &pkt, msg_len);
		spin_unlock_irqrestore(&ds_lock, flags);

		loops = 1000;
		while (ds_var_doorbell == 0) {
			if (loops-- < 0)
				break;
			barrier();
			udelay(100);
		}

		mutex_unlock(&ds_var_mutex);

		if (ds_var_doorbell == 0 ||
		    ds_var_response != DS_VAR_SUCCESS)
			printk(KERN_ERR PFX "var-config [%s:%s] "
			       "failed, response(%d).\n",
			       var, value,
			       ds_var_response);
	} else {
		printk(KERN_ERR PFX "var-config not registered so "
		       "could not set (%s) variable to (%s).\n",
		       var, value);
	}
}

void ldom_reboot(const char *boot_command)
{
	/* Don't bother with any of this if the boot_command
	 * is empty.
	 */
	if (boot_command && strlen(boot_command)) {
		char full_boot_str[256];

		strcpy(full_boot_str, "boot ");
		strcpy(full_boot_str + strlen("boot "), boot_command);

		ldom_set_var("reboot-command", full_boot_str);
	}
	sun4v_mach_sir();
}

void ldom_power_off(void)
{
	sun4v_mach_exit(0);
}

static void ds_conn_reset(struct ds_info *dp)
{
	printk(KERN_ERR PFX "ds_conn_reset() from %p\n",
	       __builtin_return_address(0));
}

static int register_services(struct ds_info *dp)
{
	struct ldc_channel *lp = dp->lp;
	int i;

	for (i = 0; i < ARRAY_SIZE(ds_states); i++) {
		struct {
			struct ds_reg_req req;
			u8 id_buf[256];
		} pbuf;
		struct ds_cap_state *cp = &ds_states[i];
		int err, msg_len;
		u64 new_count;

		if (cp->state == CAP_STATE_REGISTERED)
			continue;

		new_count = sched_clock() & 0xffffffff;
		cp->handle = ((u64) i << 32) | new_count;

		msg_len = (sizeof(struct ds_reg_req) +
			   strlen(cp->service_id));

		memset(&pbuf, 0, sizeof(pbuf));
		pbuf.req.tag.type = DS_REG_REQ;
		pbuf.req.tag.len = (msg_len - sizeof(struct ds_msg_tag));
		pbuf.req.handle = cp->handle;
		pbuf.req.major = 1;
		pbuf.req.minor = 0;
		strcpy(pbuf.req.svc_id, cp->service_id);

		err = ds_send(lp, &pbuf, msg_len);
		if (err > 0)
			cp->state = CAP_STATE_REG_SENT;
	}
	return 0;
}

static int ds_handshake(struct ds_info *dp, struct ds_msg_tag *pkt)
{

	if (dp->hs_state == DS_HS_START) {
		if (pkt->type != DS_INIT_ACK)
			goto conn_reset;

		dp->hs_state = DS_HS_DONE;

		return register_services(dp);
	}

	if (dp->hs_state != DS_HS_DONE)
		goto conn_reset;

	if (pkt->type == DS_REG_ACK) {
		struct ds_reg_ack *ap = (struct ds_reg_ack *) pkt;
		struct ds_cap_state *cp = find_cap(ap->handle);

		if (!cp) {
			printk(KERN_ERR PFX "REG ACK for unknown handle %lx\n",
			       ap->handle);
			return 0;
		}
		printk(KERN_INFO PFX "Registered %s service.\n",
		       cp->service_id);
		cp->state = CAP_STATE_REGISTERED;
	} else if (pkt->type == DS_REG_NACK) {
		struct ds_reg_nack *np = (struct ds_reg_nack *) pkt;
		struct ds_cap_state *cp = find_cap(np->handle);

		if (!cp) {
			printk(KERN_ERR PFX "REG NACK for "
			       "unknown handle %lx\n",
			       np->handle);
			return 0;
		}
		printk(KERN_INFO PFX "Could not register %s service\n",
		       cp->service_id);
		cp->state = CAP_STATE_UNKNOWN;
	}

	return 0;

conn_reset:
	ds_conn_reset(dp);
	return -ECONNRESET;
}

static int ds_data(struct ds_info *dp, struct ds_msg_tag *pkt, int len)
{
	struct ds_data *dpkt = (struct ds_data *) pkt;
	struct ds_cap_state *cp = find_cap(dpkt->handle);

	if (!cp) {
		struct ds_data_nack nack = {
			.tag = {
				.type = DS_NACK,
				.len = (sizeof(struct ds_data_nack) -
					sizeof(struct ds_msg_tag)),
			},
			.handle = dpkt->handle,
			.result = DS_INV_HDL,
		};

		printk(KERN_ERR PFX "Data for unknown handle %lu\n",
		       dpkt->handle);
		ds_send(dp->lp, &nack, sizeof(nack));
	} else {
		cp->data(dp->lp, cp, dpkt, len);
	}
	return 0;
}

static void ds_up(struct ds_info *dp)
{
	struct ldc_channel *lp = dp->lp;
	struct ds_ver_req req;
	int err;

	req.tag.type = DS_INIT_REQ;
	req.tag.len = sizeof(req) - sizeof(struct ds_msg_tag);
	req.ver.major = 1;
	req.ver.minor = 0;

	err = ds_send(lp, &req, sizeof(req));
	if (err > 0)
		dp->hs_state = DS_HS_START;
}

static void ds_event(void *arg, int event)
{
	struct ds_info *dp = arg;
	struct ldc_channel *lp = dp->lp;
	unsigned long flags;
	int err;

	spin_lock_irqsave(&ds_lock, flags);

	if (event == LDC_EVENT_UP) {
		ds_up(dp);
		spin_unlock_irqrestore(&ds_lock, flags);
		return;
	}

	if (event != LDC_EVENT_DATA_READY) {
		printk(KERN_WARNING PFX "Unexpected LDC event %d\n", event);
		spin_unlock_irqrestore(&ds_lock, flags);
		return;
	}

	err = 0;
	while (1) {
		struct ds_msg_tag *tag;

		err = ldc_read(lp, dp->rcv_buf, sizeof(*tag));

		if (unlikely(err < 0)) {
			if (err == -ECONNRESET)
				ds_conn_reset(dp);
			break;
		}
		if (err == 0)
			break;

		tag = dp->rcv_buf;
		err = ldc_read(lp, tag + 1, tag->len);

		if (unlikely(err < 0)) {
			if (err == -ECONNRESET)
				ds_conn_reset(dp);
			break;
		}
		if (err < tag->len)
			break;

		if (tag->type < DS_DATA)
			err = ds_handshake(dp, dp->rcv_buf);
		else
			err = ds_data(dp, dp->rcv_buf,
				      sizeof(*tag) + err);
		if (err == -ECONNRESET)
			break;
	}

	spin_unlock_irqrestore(&ds_lock, flags);
}

static int __devinit ds_probe(struct vio_dev *vdev,
			      const struct vio_device_id *id)
{
	static int ds_version_printed;
	struct ldc_channel_config ds_cfg = {
		.event		= ds_event,
		.mtu		= 4096,
		.mode		= LDC_MODE_STREAM,
	};
	struct ldc_channel *lp;
	struct ds_info *dp;
	int err;

	if (ds_version_printed++ == 0)
		printk(KERN_INFO "%s", version);

	dp = kzalloc(sizeof(*dp), GFP_KERNEL);
	err = -ENOMEM;
	if (!dp)
		goto out_err;

	dp->rcv_buf = kzalloc(4096, GFP_KERNEL);
	if (!dp->rcv_buf)
		goto out_free_dp;

	dp->rcv_buf_len = 4096;

	ds_cfg.tx_irq = vdev->tx_irq;
	ds_cfg.rx_irq = vdev->rx_irq;

	lp = ldc_alloc(vdev->channel_id, &ds_cfg, dp);
	if (IS_ERR(lp)) {
		err = PTR_ERR(lp);
		goto out_free_rcv_buf;
	}
	dp->lp = lp;

	err = ldc_bind(lp, "DS");
	if (err)
		goto out_free_ldc;

	ds_info = dp;

	start_powerd();

	return err;

out_free_ldc:
	ldc_free(dp->lp);

out_free_rcv_buf:
	kfree(dp->rcv_buf);

out_free_dp:
	kfree(dp);

out_err:
	return err;
}

static int ds_remove(struct vio_dev *vdev)
{
	return 0;
}

static struct vio_device_id ds_match[] = {
	{
		.type = "domain-services-port",
	},
	{},
};

static struct vio_driver ds_driver = {
	.id_table	= ds_match,
	.probe		= ds_probe,
	.remove		= ds_remove,
	.driver		= {
		.name	= "ds",
		.owner	= THIS_MODULE,
	}
};

static int __init ds_init(void)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(ds_states); i++)
		ds_states[i].handle = ((u64)i << 32);

#ifdef CONFIG_HOTPLUG_CPU
	kthread_run(dr_cpu_thread, NULL, "kdrcpud");
#endif

	return vio_register_driver(&ds_driver);
}

subsys_initcall(ds_init);