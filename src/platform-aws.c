/*
 * Copyright (c) 2018-2024 Amazon.com, Inc. or its affiliates. All rights reserved.
 * Copyright (c) 2015-2018, NVIDIA CORPORATION. All rights reserved.
 */

#include "config.h"

#define _GNU_SOURCE
#include <alloca.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <inttypes.h>
#ifdef HAVE_RDMA_FI_EXT_H
#include <rdma/fi_ext.h>
#endif
#include <dlfcn.h>

#include "nccl_ofi.h"
#include "nccl_ofi_platform.h"
#include "nccl_ofi_log.h"
#include "nccl_ofi_math.h"
#include "nccl_ofi_rdma.h"
#include "nccl_ofi_param.h"
#include "nccl_ofi_pthread.h"

struct ec2_platform_data {
	const char* name;
	const char* topology;
	int default_dup_conns;
	float latency;
	bool gdr_required;
	bool net_flush_required;
	const char *default_protocol;
	int domain_per_thread;
} platform_data_map[] = {
	{
		.name = "p4d.24xlarge",
		.topology = "p4d-24xl-topo.xml",
		.default_dup_conns = 0,
		.latency = 75.0,
		.gdr_required = true,
		.net_flush_required = true,
		.default_protocol = "SENDRECV",
		.domain_per_thread = 0,
	},
	{
		.name = "p4de.24xlarge",
		.topology = "p4de-24xl-topo.xml",
		.default_dup_conns = 0,
		.latency = 75.0,
		.gdr_required = true,
		.net_flush_required = true,
		.default_protocol = "SENDRECV",
		.domain_per_thread = 0,
	},
	{
		.name = "p3dn.24xlarge",
		.topology = NULL,
		.default_dup_conns = 4,
		.latency = 150.0,
		.gdr_required = false,
		.net_flush_required = true,
		.default_protocol = "SENDRECV",
		.domain_per_thread = 0,
	},
	{
		.name = "p5.48xlarge",
		.topology = "p5.48xl-topo.xml",
		.default_dup_conns = 0,
		.latency = 75.0,
		.gdr_required = true,
		.net_flush_required = false,
		.default_protocol = "RDMA",
		.domain_per_thread = 0,
	},
	{
		.name = "g5.48xlarge",
		.topology = "g5.48xl-topo.xml",
		.gdr_required = false,
		.net_flush_required = true,
		.default_protocol = "SENDRECV",
		.domain_per_thread = 0,
	},
	{
		.name = "trn1.32xlarge",
		.default_protocol = "SENDRECV",
		.gdr_required = true,
		.net_flush_required = true,
		.domain_per_thread = 1,
	},
	{
		.name = "trn1n.32xlarge",
		.default_protocol = "SENDRECV",
		.gdr_required = true,
		.net_flush_required = true,
		.domain_per_thread = 1,
	}
};

/*
 * @brief	Provides EC2 platform type as reported by the
 * 		first line of
 *		/sys/devices/virtual/dmi/id/product_name.
 *		Users of this API *should* free the buffer when a
 *		Non-NULL string is returned.
 *
 * @return	NULL, on allocation and file system error
 * 		EC2 platform type, on success
 */
static const char* get_platform_type(void)
{
	char file[] = "/sys/devices/virtual/dmi/id/product_name";
	FILE *fd = NULL;
	char ch;
	size_t len = 0;
	size_t platform_type_len = 64;
	static bool init = false;
	static char *platform_type = NULL;
	static pthread_mutex_t platform_mutex = PTHREAD_MUTEX_INITIALIZER;

	nccl_net_ofi_mutex_lock(&platform_mutex);

	if (init) {
		nccl_net_ofi_mutex_unlock(&platform_mutex);
		return platform_type;
	}

	init = true;

	fd = fopen(file, "r");
	if (fd == NULL) {
		NCCL_OFI_WARN("Error opening file: %s", file);
		goto error;
	}

	platform_type = (char *)malloc(sizeof(char)*platform_type_len);
	if (platform_type == NULL) {
		NCCL_OFI_WARN("Unable to allocate platform type");
		goto error;
	}

	/* Read first line of the file, reallocing the buffer as necessary */
	while ((feof(fd) == 0) && (ferror(fd) == 0) && ((ch = fgetc(fd)) != '\n')) {
		platform_type[len++] = ch;
		if (len >= platform_type_len) {
			platform_type = (char*)realloc(platform_type, len + platform_type_len);
		}
	}

	platform_type[len] = '\0';

	if (ferror(fd)) {
		NCCL_OFI_WARN("Error reading file: %s", file);
		goto error;
	}

	NCCL_OFI_TRACE(NCCL_INIT | NCCL_NET, "EC2 platform type is %s", platform_type);

	goto exit;

error:
	if (platform_type) {
		free(platform_type);
		platform_type = NULL;
	}

exit:
	if (fd)
		fclose(fd);

	nccl_net_ofi_mutex_unlock(&platform_mutex);

	return platform_type;
}

/*
 * @brief	Returns platform data for current platform type, if found
 *
 * @input	Platform type
 *
 * @return	NULL, if no topology found
 * 		platform data, if match found
 */
struct ec2_platform_data *get_platform_data()
{
	static bool init = false;
	static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
	static struct ec2_platform_data *platform_data = NULL;
	const size_t platform_n = sizeof(platform_data_map)/sizeof(platform_data_map[0]);
	const char* platform_type = NULL;

	nccl_net_ofi_mutex_lock(&mutex);

	if (init) {
		nccl_net_ofi_mutex_unlock(&mutex);
		return platform_data;
	}
	init = true;

	platform_type = get_platform_type();
	if (platform_type == NULL) {
		nccl_net_ofi_mutex_unlock(&mutex);
		return NULL;
	}

	for (size_t idx = 0; idx < platform_n; idx++) {
		if (strcmp(platform_type, platform_data_map[idx].name) == 0)
			platform_data = &platform_data_map[idx];
	}

	nccl_net_ofi_mutex_unlock(&mutex);

	return platform_data;
}

/*
 * validate that EFA is using RDMA write natively and not in an
 * emulated fasion.
 */
static int validate_rdma_write(struct fid_ep *ep)
{
	int ret = 0;
#if HAVE_DECL_FI_OPT_EFA_EMULATED_WRITE
	bool optval;
	size_t optlen = sizeof(optval);

	ret = fi_getopt(&ep->fid, FI_OPT_ENDPOINT, FI_OPT_EFA_EMULATED_WRITE, &optval, &optlen);
	if (ret != 0) {
		NCCL_OFI_WARN("Couldn't get FI_OPT_EFA_EMULATED_WRITE. RC: %d, ERROR: %s",
			      ret, fi_strerror(-ret));
		goto exit;
	} else if (optlen != sizeof(optval)) {
		NCCL_OFI_WARN("Unexpected response size when checking FI_OPT_EFA_EMULATED_WRITE.  Expected %lu, got %lu",
			      sizeof(optval), optlen);
		ret = -EINVAL;
		goto exit;
	} else if (optval) {
		NCCL_OFI_WARN("FI_OPT_EFA_EMULATED_WRITE is true when the communication protocol is RDMA write.");
		ret = -EINVAL;
		goto exit;
	}
	NCCL_OFI_TRACE(NCCL_INIT | NCCL_NET, "Get endpoint option FI_OPT_EFA_EMULATED_WRITE. optval: %d", 
		       optval);
#else
	NCCL_OFI_WARN("FI_OPT_EFA_EMULATED_WRITE not declared when the communication protocol is RDMA write.");
	ret = -EINVAL;
	goto exit;
#endif

exit:
	return ret;
}


#if HAVE_CUDA
static int configure_nccl_proto(void)
{
	int ret;

	if (!getenv("NCCL_PROTO")) {
		NCCL_OFI_INFO(NCCL_INIT, "Setting NCCL_PROTO to \"simple\"");
		ret = setenv("NCCL_PROTO", "simple", 0);
		if (ret != 0) {
			NCCL_OFI_WARN("Error setting NCCL_PROTO environment variable: %s",
				      strerror(errno));
			return -errno;
		}
	} else if (strcasecmp(getenv("NCCL_PROTO"), "simple") != 0) {
		NCCL_OFI_WARN("NCCL_PROTO was set to \"LL/LL128\", but the Libfabric endpoint does not support 128 byte in-order aligned stores. This endpoint may corrupt data during communication");
	}

	return 0;
}

/*
 * Try to set one of the in-order flags for either send/recv or rdma
 * on the current endpoint to true.  have_ordering will be the
 * returned value on output.
 *
 * Returns 0 on success (ie, have_ordering is in a sane state) or
 * -error code on unexpected failure.
 */
static int configure_ep_inorder(struct fid_ep *ep, int optname, const char* optname_name,
				bool *have_ordering)
{
#if HAVE_DECL_FI_OPT_EFA_WRITE_IN_ORDER_ALIGNED_128_BYTES
	int ret = 0;
	bool optval = true;

	*have_ordering = false;

	ret = fi_setopt(&ep->fid, FI_OPT_ENDPOINT,
			optname, &optval, sizeof(optval));
	if (ret == -FI_EOPNOTSUPP || ret == -FI_ENOPROTOOPT) {
		NCCL_OFI_INFO(NCCL_INIT, "Setting %s not supported.", optname_name);
	} else if (ret != 0) {
		NCCL_OFI_WARN("Could not set %s. RC: %d, ERROR: %s",
			      optname_name, ret, fi_strerror(-ret));
		return ret;
	} else {
		*have_ordering = true;
	}

	NCCL_OFI_TRACE(NCCL_INIT, "fi_setopt(%s) ordering result %s, error code %d",
		       optname_name, have_ordering ? "yes" : "no", ret);
#else
	*have_ordering = false;
#endif
	return 0;
}

/*
 * For the RDMA protocol, try to set max msg size on the current endpoint
 * to the size of the max message we send with fi_send. This allows the EFA
 * provider to enable the zero-copy path.
 *
 * Returns 0 on success or -error code on unexpected failure.
 */
static int configure_ep_max_msg_size(struct fid_ep *ep)
{
	int ret = 0;

#if HAVE_DECL_FI_OPT_MAX_MSG_SIZE
	size_t eager_max_size = (size_t)ofi_nccl_eager_max_size();
	size_t optval = NCCL_OFI_MAX(NCCL_OFI_MAX(sizeof(nccl_net_ofi_rdma_ctrl_msg_t), eager_max_size),
				     sizeof(nccl_ofi_rdma_connection_info_t));

	ret = fi_setopt(&ep->fid, FI_OPT_ENDPOINT, FI_OPT_MAX_MSG_SIZE, &optval, sizeof(optval));

	NCCL_OFI_TRACE(NCCL_INIT, "fi_setopt(FI_OPT_MAX_MSG_SIZE) RC: %d", ret);

	if (ret == -FI_EOPNOTSUPP || ret == -FI_ENOPROTOOPT) {
		NCCL_OFI_INFO(NCCL_INIT, "Setting FI_OPT_MAX_MSG_SIZE not supported.");
		ret = 0;
	} else if (ret != 0) {
		NCCL_OFI_WARN("Could not set FI_OPT_MAX_MSG_SIZE. RC: %d, ERROR: %s", ret, fi_strerror(-ret));
	}
#endif

	return ret;
}


typedef ncclResult_t (*nccl_get_version_fn_t)(int *version);

int configure_nvls_option(void)
{
	/* Disable NVLS topology discovery for older NCCL versions. There's a
	 * bug with EFA and NCCL version 2.18.3 and earlier on platforms with
	 * NVLink Switch support.  We selectively disable NVLS support
	 * to avoid the bug, which was fixed in 2.18.5.
	 */
	nccl_get_version_fn_t nccl_get_version = NULL;
	int version = 0;
	ncclResult_t nccl_ret;
	int ret;

	if (getenv("NCCL_NVLS_ENABLE") == NULL) {
		nccl_get_version = (nccl_get_version_fn_t)dlsym(RTLD_DEFAULT, "ncclGetVersion");
		if (nccl_get_version == NULL) {
			NCCL_OFI_TRACE(NCCL_INIT | NCCL_NET,
			    "Could not find ncclGetVersion symbol; skipping NVLS NCCL version check");
			return 0;
		} else {
			nccl_ret = nccl_get_version(&version);
			if (nccl_ret != ncclSuccess) {
				NCCL_OFI_WARN("ncclGetVersion returned %d", nccl_ret);
				return -ENOTSUP;
			}

			NCCL_OFI_TRACE(NCCL_INIT | NCCL_NET, "ncclGetVersion results = %lu", version);
		}

		/* 2.18.5 */
		if (version < 21805) {
			NCCL_OFI_INFO(NCCL_INIT | NCCL_NET, "Disabling NVLS support due to NCCL version %lu", version);
			ret = setenv("NCCL_NVLS_ENABLE", "0", 1);
			if (ret != 0) {
				NCCL_OFI_WARN("Unable to set NCCL_NVLS_ENABLE");
				return -errno;
			}
		} else {
			NCCL_OFI_TRACE(NCCL_INIT | NCCL_NET, "Not disabling NVLS support due to NCCL version %lu", version);
		}
	}

	return 0;
}

#endif /* HAVE_CUDA */

/*
 * @brief	Update NCCL's system topology using static pre-configured topology
 * 		files for supported EC2 platform types.
 *
 * @return	0, when we are succesfully able to update NCCL topology or
 * 		   if we find no match
 * 		error, on failure
 */
int platform_init(const char **provider_filter)
{
	int ret = ncclSuccess;
	struct ec2_platform_data *platform_data;
	bool select_efa = false;
	char *fi_provider;

	NCCL_OFI_INFO(NCCL_INIT, "Configuring AWS-specific options");

	platform_data = get_platform_data();

	/* if we're here, we think we're on an EC2 instance, so force
	 * EFA provider (for platforms without EFA, this will cause a
	 * fallback to NCCL's internal TCP.  In the case of Neuron, a
	 * hard failure when there are no NICs.  Both are the
	 * behaviors we want).
	 */
	fi_provider = getenv("FI_PROVIDER");
	if (fi_provider == NULL) {
		NCCL_OFI_INFO(NCCL_INIT, "Setting provider_filter to efa");
		*provider_filter = "efa";
		select_efa = true;
	} else if (0 == strcmp(fi_provider, "efa")) {
		select_efa = true;
	}

#if HAVE_CUDA
	/*
	 * FI_EFA_FORK_SAFE environment variable tells Libfabric to enable
	 * fork-safe support in legacy versions of the rdma-core library.
	 * Libfabric checks if additional handling is required for fork safety,
	 * and does not introduce this additional overhead of setting MADV_DONTFORK
	 * for new versions of rdma-core (38.0 and later) and the Linux kernel
	 * that support copy-on-fork for pinned memory (5.13 and later).
	 * These new versions are always fork-safe and additional support in userspace
	 * is not required.
	 *
	 * When legacy versions of the kernel and rdma-core are used, setting
	 * FI_EFA_FORK_SAFE to 1 disables the use of huge pages in Libfabric.
	 *
	 * To prevent data corruption, the EFA provider registers an atfork
	 * handler which will abort the process whenever it believes
	 * rdma-core is not fork-safe.
	 *
	 * NCCL applications heavily re-use the buffers for communication and
	 * thus are not sensitive to increased memory registration costs.
	 * To prevent NCCL based applications from getting aborted when using
	 * fork(), the plugin explicitly enables FI_EFA_FORK_SAFE environment
	 * variable, even in legacy environments where the overhead is high.
	 *
	 * The Neuron team has asked us to skip trying to set this
	 * environment variable on Neuron platforms, so we only do
	 * this for Nvidia platforms.
	 */
	uint32_t libversion = fi_version();
	const char * fork_safe_var_name =
		(FI_MAJOR(libversion) > 1 || (FI_MAJOR(libversion) == 1 && FI_MINOR(libversion) >= 13))
		? "FI_EFA_FORK_SAFE"
		: "RDMAV_FORK_SAFE";
	if (!getenv(fork_safe_var_name)) {
		NCCL_OFI_INFO(NCCL_INIT, "Setting %s environment variable to 1", fork_safe_var_name);
		ret = setenv(fork_safe_var_name, "1", 1);
		if (ret != 0) {
			NCCL_OFI_WARN("Unable to set %s", fork_safe_var_name);
			ret = -errno;
			goto exit;
		}
	}

	ret = configure_nvls_option();
	if (ret != 0) {
		NCCL_OFI_WARN("Unable to configure NVLS option");
		goto exit;
	}

	if ((platform_data && !platform_data->net_flush_required) &&
	    NULL == getenv("NCCL_NET_FORCE_FLUSH")) {

		/* Hopper GPUs do not require a network flush, but NCCL versions <2.19.1
		* still enable flush by default on any GPU type.
		* For GPU generations earlier than Hopper, NCCL always enables flush, while
		* for Hopper GPUs flush is enabled or disabled depending on the value of
		* the NCCL_NET_FORCE_FLUSH environment variable. The default value for this
		* variable is 1 for NCCL versions <2.19.1, which forces flush when it is not
		* needed, so it is safe to set it to 0 if it is not explicitly set.
		*/

		NCCL_OFI_INFO(NCCL_INIT | NCCL_NET, "Setting NCCL_NET_FORCE_FLUSH=0 for Hopper GPUs");
		ret = setenv("NCCL_NET_FORCE_FLUSH", "0", 0);
		if (ret != 0) {
			NCCL_OFI_WARN("Unable to set NCCL_NET_FORCE_FLUSH");
			ret = -errno;
			goto exit;
		}
	}

	/*
	 * NCCL v2.19.3 reduced the chunk size used when running NVLS Tree
	 * algorithm on greater than 4 nodes to 64KiB. This drastically impacted
	 * performance on AWS (Ref: https://github.com/NVIDIA/nccl/pull/1112/
	 * for some data). NCCL v2.20.3 has made this a tunable. Based on
	 * empirical testing, a max chunk size of 512KiB recovers from the
	 * regression and was also observed to be the default in v2.19.3.
	 * Setting this unconditionally without relying on ncclGetVersion symbol
	 * being available, since the parameter did not exist in versions prior
	 * to v2.20.
	 *
	 * The NVLSTree chunk size can not be larger than the NVLS chunk size,
	 * so we ensure both are set to 512KiB.
	 */
	NCCL_OFI_INFO(NCCL_INIT | NCCL_NET, "Setting NCCL_NVLSTREE_MAX_CHUNKSIZE to 512KiB");
	ret = setenv("NCCL_NVLSTREE_MAX_CHUNKSIZE", "524288", 0);
	if (ret != 0) {
		NCCL_OFI_WARN("Unable to set NCCL_NVLSTREE_MAX_CHUNKSIZE");
		ret = -errno;
		goto exit;
	}

	NCCL_OFI_INFO(NCCL_INIT | NCCL_NET, "Setting NCCL_NVLS_CHUNKSIZE to 512KiB");
	ret = setenv("NCCL_NVLS_CHUNKSIZE", "524288", 0);
	if (ret != 0) {
		NCCL_OFI_WARN("Unable to set NCCL_NVLS_CHUNKSIZE");
		ret = -errno;
		goto exit;
	}
#endif

	/*
	 * Update topology if platform topology is available and 
	 * environment variable NCCL_TOPO_FILE is not set.
	 */
	if (getenv("NCCL_TOPO_FILE")) {
		NCCL_OFI_INFO(NCCL_INIT | NCCL_NET,
			      "Running on %s platform, NCCL_TOPO_FILE environment variable is already set to %s",
			      get_platform_type(), getenv("NCCL_TOPO_FILE"));
	} else if (platform_data && platform_data->topology) {
		char topology_path[PATH_MAX];

		ret = snprintf(topology_path, sizeof(topology_path), "%s/%s",
			       XML_DIR, platform_data->topology);
		if (ret < 0 || ret >= sizeof(topology_path)) {
			NCCL_OFI_WARN("Error occurred while forming the complete topology XML file path. RC: %d, Buffer Size: %d, XML dir: %s, Topology file: %s",
				      ret, PATH_MAX, XML_DIR, platform_data->topology);
			ret = -ENOMEM;
			goto exit;
		}

		NCCL_OFI_INFO(NCCL_INIT | NCCL_NET,
				"Running on %s platform, Setting NCCL_TOPO_FILE environment variable to %s",
				get_platform_type(), topology_path);

		ret = setenv("NCCL_TOPO_FILE", topology_path, 1);
		if (ret != 0) {
			NCCL_OFI_WARN("Unable to set NCCL_TOPO_FILE");
			ret = -errno;
			goto exit;
		}

	}

	if (nic_dup_conns == 0 && platform_data)
		nic_dup_conns = platform_data->default_dup_conns;

	if (ofi_nccl_net_latency() < 0) {
		if (platform_data && platform_data->latency >= 0.0) {
			net_latency = platform_data->latency;
		} else {
			/* For historical reasons, default value for EFA is 150 us */
			net_latency = 150.0;
		}
		NCCL_OFI_INFO(NCCL_INIT | NCCL_NET, "Internode latency set at %.1f us",
				net_latency);
	}

	if (select_efa && ofi_nccl_protocol() == NULL && platform_data) {
		nccl_ofi_selected_protocol = platform_data->default_protocol;
	}

	domain_per_thread = ofi_nccl_domain_per_thread();
	if (domain_per_thread == -1) {
		if (platform_data != NULL) {
			domain_per_thread = platform_data->domain_per_thread;
		} else {
			domain_per_thread = 0;
		}
	}
	NCCL_OFI_INFO(NCCL_INIT | NCCL_NET, "Creating one domain per %s", domain_per_thread ? "thread" : "process");

exit:
	return ret;
}

int platform_config_endpoint(struct fi_info *info, struct fid_ep* endpoint) {
	int ret = 0;

	if (endpoint == NULL) {
		NCCL_OFI_WARN("Unable to configure invalid endpoint");
		ret = -EINVAL;
		goto exit;
	}

	/* short circuit when not using EFA */
	if (0 != strcmp(info->fabric_attr->prov_name, "efa")) {
		ret = 0;
		goto exit;
	}

	if (ofi_nccl_disable_gdr_required_check() == 0) {
		/* Ensure GDR is enabled on GDR-supported instances */
		struct ec2_platform_data *platform_data = get_platform_data();
		if (platform_data && platform_data->gdr_required && support_gdr != GDR_SUPPORTED) {
			NCCL_OFI_WARN("GDR disabled on GDR-supported instance type %s", platform_data->name);
			ret = -EINVAL;
			goto exit;
		}
	}

	/* If the selected communication protocol is RDMA write and the user did
	 * not disable the native RDMA support check, validate that the
	 * FI_OPT_EFA_EMULATED_WRITE endpoint option can be accessed, and that
	 * emulated writes are disabled.
	 */

	if (0 == strcasecmp("RDMA", nccl_ofi_selected_protocol) &&
	    ofi_nccl_disable_native_rdma_check() == 0) {
		ret = validate_rdma_write(endpoint);
		if (ret != 0) {
			goto exit;
		}
	}

#if HAVE_CUDA
	static bool nccl_proto_configured = false;
	static bool need_ordering = false;
	static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
	int optname = -1;
	const char *optname_name = "none";

	/* During initialization, try to set
	 * FI_OPT_EFA_{SENDRECV,WRTIE}_IN_ORDER_ALIGNED_128_BYTES to
	 * true to see if the LL/LL128 protocol is supported. After
	 * initialization, try to set the option to true again if it
	 * was previously set and error if we can't set them the same
	 * way later.
	 */
	if (0 == strcasecmp("SENDRECV", nccl_ofi_selected_protocol)) {
#if HAVE_DECL_FI_OPT_EFA_SENDRECV_IN_ORDER_ALIGNED_128_BYTES
		optname = FI_OPT_EFA_SENDRECV_IN_ORDER_ALIGNED_128_BYTES;
		optname_name = "FI_OPT_EFA_SENDRECV_IN_ORDER_ALIGNED_128_BYTES";
#endif
	} else if (0 == strcasecmp("RDMA", nccl_ofi_selected_protocol)) {
#if HAVE_DECL_FI_OPT_EFA_WRITE_IN_ORDER_ALIGNED_128_BYTES
		optname = FI_OPT_EFA_WRITE_IN_ORDER_ALIGNED_128_BYTES;
		optname_name = "FI_OPT_EFA_WRITE_IN_ORDER_ALIGNED_128_BYTES";
#endif
	} else {
		NCCL_OFI_WARN("unkonwn transport %s", nccl_ofi_selected_protocol);
		ret = -EINVAL;
		goto exit;
	}

	nccl_net_ofi_mutex_lock(&mutex);
	/* If we know we need byte delivery ordering (need_ordering ==
	 * true) or this is the first time that we're configuring an
	 * endpoint (nccl_proto_configured == false), then try to
	 * configure ordering on the endpoint.  The only time we care
	 * about ordering is if we don't set NCCL_PROTO=simple,
	 * because previous endpoints were able to be configured with
	 * ordering.  If we're not expecting ordering, we don't really
	 * care if ordering is on or off for the endpoint.
	 */

	/* TODO: This is a temporary hack to disable setting
	 * NCCL_PROTO=simple on P5 when using the RDMA protocol.  EFA
	 * on P5 does not currently report
	 * WRITE_IN_ORDER_ALIGNED_128_BYTES because it can deliver the
	 * (correct) payload twice.  This violates the meaning of the
	 * WRITE_IN_ORDER_ALIGNED_128_BYTES flag in rdma-core, but
	 * does not violate any assumptions about buffer reuse in
	 * NCCL.  We have confirmed that the EFA provider in Libfabric
	 * will not segment messages for fi_write(), so this is safe.
	 * Note that the SENDRECV protocol does have segmentation
	 * challenges that require us to obey the
	 * SENDRECV_IN_ORDER_ALIGNED_128_BYTES flag, so we only skip
	 * the check when using the RDMA protocol.
	 */
	if ((NULL == getenv("NCCL_PROTO")) &&
	    (0 == strcasecmp("RDMA", nccl_ofi_selected_protocol)) &&
	    (0 == strcmp(get_platform_type(), "p5.48xlarge"))) {
		if (!nccl_proto_configured) {
			NCCL_OFI_INFO(NCCL_INIT, "Skipping NCCL_PROTO checks on P5 + RDMA");
			need_ordering = false;
			nccl_proto_configured = true;
		}
	}

	if (need_ordering || !nccl_proto_configured) {
		bool have_ordering = false;

		if (optname != -1) {
			ret = configure_ep_inorder(endpoint, optname, optname_name,
						   &have_ordering);
			if (ret != 0) {
				NCCL_OFI_WARN("Unexpected failure setting inorder %d", ret);
				goto unlock;
			}
		}

		if (need_ordering && !have_ordering) {
			NCCL_OFI_WARN("Setting %s option failed after succeeding during initialization",
				      optname_name);
			ret = -ENOTSUP;
			goto unlock;
		}

		if (!nccl_proto_configured) {
			need_ordering = have_ordering;
			nccl_proto_configured = true;

			if (!have_ordering) {
				ret = configure_nccl_proto();
				if (ret != 0) {
					NCCL_OFI_WARN("Failed to set NCCL_PROTO: %d", ret);
					ret = -ENOTSUP;
					goto unlock;
				}
			}
		}
	}

	if (0 == strcasecmp("RDMA", nccl_ofi_selected_protocol)) {
		ret = configure_ep_max_msg_size(endpoint);
		if (ret != 0) {
			NCCL_OFI_WARN("Unexpected failure setting max_msg_size %d", ret);
			goto unlock;
		}
	}

unlock:
	nccl_net_ofi_mutex_unlock(&mutex);
#endif // HAVE_CUDA

exit:
	return ret;
}

static int get_rail_vf_idx(struct fi_info *info)
{
	char guid_file[256], guid[20];
	char *endptr;
	int vf_idx;
	FILE *fp;

	snprintf(guid_file, sizeof(guid_file), "/sys/class/infiniband/%s/node_guid",
		info->nic->device_attr->name);
	fp = fopen(guid_file, "r");
	if (fp == NULL) {
		NCCL_OFI_WARN("Error opening file: %s", guid_file);
		return -EIO;
	}

	if (fgets(guid, sizeof(guid), fp) == NULL) {
		NCCL_OFI_WARN("Error reading file: %s", guid_file);
		fclose(fp);
		return -EIO;
	}
	fclose(fp);

	/**
	 * GUID is a 64-bit hex number with format:
	 *
	 * XXXX:XXXX:XXXX:XXXX
	 *
	 * The lowest 8 bits are the VF id.
	 */
	if (strlen(guid) != 19) {
		NCCL_OFI_WARN("Bad GUID format: wrong size: %s", guid);
		return -EINVAL;
	}

	if (guid[14] != ':') {
		NCCL_OFI_WARN("Bad GUID format: wrong colon pos: %s", guid);
		return -EINVAL;
	}
	/* guid[14...] string should now have format ":XXXX". Extract the final two digits
	   as the vf idx */
	vf_idx = (int)strtol(guid + 17, &endptr, 10);
	if (endptr != guid + 19) {
		/* No valid conversion was performed */
		NCCL_OFI_WARN("Can't locate vf_idx in GUID %s", guid);
		return -EINVAL;
	}

	return vf_idx;
}

void platform_sort_rails(struct fi_info **info_list, int num_rails)
{
	struct fi_info *info_list_in = *info_list;
	struct fi_info **sorted_info_array = (struct fi_info **)alloca(num_rails*sizeof(struct fi_info *));

	if (num_rails <= 0) {
		return;
	}

	for (int i = 0; i < num_rails; ++i) {
		sorted_info_array[i] = NULL;
	}

	int rail_map[2] = {0, 2};

	for (int i = 0; i < num_rails; ++i) {
		if (info_list_in == NULL) {
			goto error;
		}

		int vf_idx = get_rail_vf_idx(info_list_in);
		if (vf_idx < 0 || vf_idx >= 2) {
			NCCL_OFI_WARN("Invalid vf_idx value %d", vf_idx);
			goto error;
		}

		int rail_idx = rail_map[vf_idx];
		rail_map[vf_idx]++;

		NCCL_OFI_TRACE(NCCL_INIT | NCCL_NET, "Assigning rail index %d to info list idx %d",
			rail_idx, i);

		if (sorted_info_array[rail_idx]) {
			NCCL_OFI_WARN("Attempted to fill rail slot with duplicate infos");
			goto error;
		}
		sorted_info_array[rail_idx] = info_list_in;

		info_list_in = info_list_in->next;
	}

	/* Update info_list references to match sorted order */
	*info_list = sorted_info_array[0];
	struct fi_info *info_ptr = *info_list;
	for (int i = 0; i < num_rails; ++i) {
		assert(info_ptr);
		assert(sorted_info_array[i]);
		if (i == num_rails - 1) {
			info_ptr->next = NULL;
		} else {
			info_ptr->next = sorted_info_array[i+1];
		}
		info_ptr = info_ptr->next;
	}

error:
	return;
}
