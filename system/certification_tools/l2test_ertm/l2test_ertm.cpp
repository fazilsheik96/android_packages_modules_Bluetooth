/******************************************************************************
 ** Copyright (c) 2014-2015, The Linux Foundation. All rights reserved.
 *
 * Not a Contribution.
 *
 *  Copyright (C) 2009-2012 Broadcom Corporation
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at:
 *
 *  http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.

 * Changes from Qualcomm Innovation Center are provided under the following
 * license:
 *
 * Copyright (c) 2024 Qualcomm Innovation Center, Inc. All rights reserved.
 *
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 *******************************************************************************/

/************************************************************************************
 *  Filename:      l2test_ertm.c
 *
 *  Description:   Bluedroid L2CAP TOOL application
 ***********************************************************************************/

#include <android/binder_ibinder.h>
#include <android/binder_manager.h>
#include <android/binder_process.h>
#include <android/log.h>
#include <arpa/inet.h>
#include <binder/ProcessState.h>
#include <btm_sec_api_types.h>
#include <ctype.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <hardware/bluetooth.h>
#include <hardware/bt_vendor.h>
#include <hardware/hardware.h>
#include <netdb.h>
#include <netinet/in.h>
#include <private/android_filesystem_config.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/capability.h>
#include <sys/prctl.h>
#include <time.h>
#include <unistd.h>

#include "bt_target.h"
#include <bt_testapp.h>
#include "stack/include/l2c_api.h"
#include "stack/include/l2cdefs.h"

/************************************************************************************
**  Constants & Macros
************************************************************************************/

#define PID_FILE "/data/.bdt_pid"

#ifndef MAX
#define MAX(x, y) ((x) > (y) ? (x) : (y))
#endif

#define TRANSPORT_BREDR 1  // Add tranport parameter to create bond

#define L2CAP_FCR_CHAN_OPT_STREAM (1 << L2CAP_FCR_STREAM_MODE)
#define L2CAP_FCR_STREAM_MODE 0x04
/************************************************************************************
**  Local type definitions
************************************************************************************/

enum { DISCONNECT, CONNECTING, CONNECTED, DISCONNECTING };

static int g_ConnectionState = DISCONNECT;
static int g_AdapterState = BT_STATE_OFF;
static int g_PairState = BT_BOND_STATE_NONE;
static uint16_t g_SecLevel = 0;
static bool g_SecOnlyMode = FALSE;
static int g_secvalue = 0;
static bool g_ConnType = TRUE;  // DUT is initiating connection
static bool g_Fcr_Present = FALSE;
static bool g_Sar_Present = FALSE;
static bool strict_mode = FALSE;
static uint8_t g_Fcr_Mode = L2CAP_FCR_BASIC_MODE;
static uint8_t g_Ertm_AllowedMode =
    (L2CAP_FCR_CHAN_OPT_BASIC | L2CAP_FCR_CHAN_OPT_ERTM |
     L2CAP_FCR_CHAN_OPT_STREAM);
static int g_LocalBusy = 0;

enum {
  BT_TURNON_CMD,
  BT_TURNOFF_CMD,
  I_CONNECT_CMD,
  O_CONNECT_CMD,
  DISCONNECT_CMD,
  SEND_DATA_CMD,
  O_PAIR_CMD
};

enum {
  SEND,
  RECEIVE,
  WAITANDSEND,
  PAIR,
  PING,
  CONNECT,
};

static unsigned char* buf;
/* Default mtu */
static int g_imtu = 672;
static int g_omtu = 0;

/* Default data size */
static long data_size = -1;
static long buffer_size = 2048;

static int master = 0;
static int auth = 0;
static int encrypt = 0;
static int secure_m4_l4 = 0;
/* Default number of frames */
static int num_frames = 1;
static int count = 1;

/* Default delay before data transfer */
static unsigned long g_delay = 1;

static char* filename = NULL;

/* Control channel eL2CAP default options */
tL2CAP_FCR_OPTS ertm_fcr_opts_def = {
    L2CAP_FCR_ERTM_MODE,
    3,     /* Tx window size */
    20,    /* Maximum transmissions before disconnecting */
    2000,  /* Retransmission timeout (2 secs) */
    12000, /* Monitor timeout (12 secs) */
    100    /* MPS segment size */
};

tL2CAP_FCR_OPTS stream_fcr_opts_def = {
    L2CAP_FCR_STREAM_MODE,
    3,     /* Tx window size */
    20,    /* Maximum transmissions before disconnecting */
    2000,  /* Retransmission timeout (2 secs) */
    12000, /* Monitor timeout (12 secs) */
    100    /* MPS segment size */
};
static tL2CAP_ERTM_INFO t_ertm_info = {0};

uint8_t do_l2cap_DataWrite(char* p, uint32_t len);

/************************************************************************************
**  Static variables
************************************************************************************/

static unsigned char main_done = 0;
static bt_status_t status;

/* Main API */

const bt_interface_t* sBtInterface = NULL;
const btvendor_interface_t* btvendorInterface = NULL;

static gid_t groups[] = {AID_NET_BT,    AID_INET, AID_NET_BT_ADMIN,
                         AID_SYSTEM,    AID_MISC, AID_SDCARD_RW,
                         AID_NET_ADMIN, AID_VPN};

const btl2cap_interface_t* sL2capInterface = NULL;

enum { L2CAP_NOT_CONNECTED, L2CAP_CONN_SETUP, L2CAP_CONNECTED };

static int L2cap_conn_state = L2CAP_NOT_CONNECTED;
static tL2CAP_CFG_INFO tl2cap_cfg_info;
static uint16_t g_PSM = 0;
static uint16_t g_lcid = 0;

/************************************************************************************
**  Static functions
************************************************************************************/

static int Send_Data();
static int WaitForCompletion(int Cmd, int Timeout);

/************************************************************************************
**  Externs
************************************************************************************/

/************************************************************************************
**  Functions
************************************************************************************/

//--------------------l2test----------------------------------------------------
btl2cap_interface_t* get_l2cap_interface(void);

static void l2test_l2c_error_ind_cb(uint16_t lcid, uint16_t result) { return; }

static void l2test_l2c_connect_ind_cb(const RawAddress& bd_addr, uint16_t lcid,
                                      uint16_t psm, uint8_t id) {
  if ((L2CAP_FCR_ERTM_MODE == g_Fcr_Mode) ||
      (L2CAP_FCR_STREAM_MODE == g_Fcr_Mode)) {
    sL2capInterface->ErtmConnectRsp(bd_addr, id, lcid, L2CAP_CONN_OK,
                                    L2CAP_CONN_OK, &t_ertm_info);
  } else {
    sL2capInterface->ConnectRsp(bd_addr, id, lcid, L2CAP_CONN_OK,
                                L2CAP_CONN_OK);
  }
  {
    tL2CAP_CFG_INFO cfg;
    memcpy(&cfg, &tl2cap_cfg_info, sizeof(tl2cap_cfg_info));
    if ((!sL2capInterface->ConfigReq(lcid, &cfg)) && cfg.fcr_present &&
        cfg.fcr.mode != L2CAP_FCR_BASIC_MODE) {
      cfg.fcr.mode = L2CAP_FCR_BASIC_MODE;
      cfg.fcr_present = FALSE;
      sL2capInterface->ConfigReq(lcid, &cfg);
    }
  }
  g_ConnectionState = CONNECT;
  g_lcid = lcid;
}

static void l2test_l2c_connect_cfm_cb(uint16_t lcid, uint16_t result) {
  if (result == L2CAP_CONN_OK) {
    L2cap_conn_state = L2CAP_CONN_SETUP;
    tL2CAP_CFG_INFO cfg;
    memcpy(&cfg, &tl2cap_cfg_info, sizeof(tl2cap_cfg_info));
    sL2capInterface->ConfigReq(lcid, &cfg);
    g_imtu = cfg.mtu;
    g_ConnectionState = CONNECT;
    g_lcid = lcid;
  }
}

static void l2test_l2c_connect_pnd_cb(uint16_t lcid) {
  g_ConnectionState = CONNECTING;
}
static void l2test_l2c_config_ind_cb(uint16_t lcid, tL2CAP_CFG_INFO* p_cfg) {
  p_cfg->result = L2CAP_CFG_OK;
  p_cfg->fcr_present = FALSE;
  if (p_cfg->mtu_present)
    g_omtu = p_cfg->mtu;
  else
    g_omtu = L2CAP_DEFAULT_MTU;
  sL2capInterface->ConfigRsp(lcid, p_cfg);
  return;
}

static void l2test_l2c_config_cfm_cb(uint16_t lcid, uint16_t initiator,
                                     tL2CAP_CFG_INFO* p_cfg) {
  /* For now, always accept configuration from the other side */
  if (p_cfg->result == L2CAP_CFG_OK) {
    printf("\nl2test_l2c_config_cfm_cb Success\n");
  } else {
    /* If peer has rejected FCR and suggested basic then try basic */
    if (p_cfg->fcr_present) {
      tL2CAP_CFG_INFO cfg;
      memcpy(&cfg, &tl2cap_cfg_info, sizeof(tl2cap_cfg_info));
      cfg.fcr_present = FALSE;
      sL2capInterface->ConfigReq(lcid, &cfg);
      // Remain in configure state
      return;
    }
    sL2capInterface->DisconnectReq(lcid);
  }
  if (0 == g_omtu) g_omtu = L2CAP_DEFAULT_MTU;
}

static void l2test_l2c_disconnect_ind_cb(uint16_t lcid, bool ack_needed) {
  if (ack_needed) {
    /* send L2CAP disconnect response */
    sL2capInterface->DisconnectRsp(lcid);
  }
  g_ConnectionState = DISCONNECTING;
  g_lcid = 0;
}
static void l2test_l2c_disconnect_cfm_cb(uint16_t lcid, uint16_t result) {
  g_ConnectionState = DISCONNECT;
  g_lcid = 0;
}
static void l2test_l2c_QoSViolationInd(const RawAddress& bd_addr) {
  printf("l2test_l2c_QoSViolationInd\n");
}
static void l2test_l2c_data_ind_cb(uint16_t lcid, BT_HDR* p_buf) {
  printf(
      "l2test_l2c_data_ind_cb:: event=%u, len=%u, offset=%u, "
      "layer_specific=%u\n",
      p_buf->event, p_buf->len, p_buf->offset, p_buf->layer_specific);
}
static void l2test_l2c_congestion_ind_cb(uint16_t lcid, bool is_congested) {
  printf("l2test_l2c_congestion_ind_cb\n");
}

static void l2test_l2c_tx_complete_cb(uint16_t lcid, uint16_t NoOfSDU) {
  printf("l2test_l2c_tx_complete_cb, cid=0x%x, SDUs=%u\n", lcid, NoOfSDU);
}

static void l2c_echo_rsp_cb(uint16_t p) { printf("Ping Response "); }

/* L2CAP callback function structure */
static tL2CAP_APPL_INFO l2test_l2c_appl = {l2test_l2c_connect_ind_cb,
                                           l2test_l2c_connect_cfm_cb,
                                           l2test_l2c_config_ind_cb,
                                           l2test_l2c_config_cfm_cb,
                                           l2test_l2c_disconnect_ind_cb,
                                           l2test_l2c_disconnect_cfm_cb,
                                           l2test_l2c_data_ind_cb,
                                           l2test_l2c_congestion_ind_cb,
                                           l2test_l2c_tx_complete_cb,
                                           l2test_l2c_error_ind_cb,
                                           NULL,
                                           NULL,
                                           NULL,
                                           NULL};

/************************************************************************************
**  Shutdown helper functions
************************************************************************************/

static void bdt_shutdown(void) {
  printf("shutdown bdroid test app\n");
  main_done = 1;
}

/*****************************************************************************
** Android's init.rc does not yet support applying linux capabilities
*****************************************************************************/

static void config_permissions(void) {
  struct __user_cap_header_struct header;
  struct __user_cap_data_struct cap[2];

  printf("set_aid_and_cap : pid %d, uid %d gid %d", getpid(), getuid(),
         getgid());

  header.pid = 0;

  prctl(PR_SET_KEEPCAPS, 1, 0, 0, 0);

  setuid(AID_BLUETOOTH);
  setgid(AID_BLUETOOTH);
  header.version = _LINUX_CAPABILITY_VERSION_3;

  cap[CAP_TO_INDEX(CAP_NET_RAW)].permitted |= CAP_TO_MASK(CAP_NET_RAW);
  cap[CAP_TO_INDEX(CAP_NET_ADMIN)].permitted |= CAP_TO_MASK(CAP_NET_ADMIN);
  cap[CAP_TO_INDEX(CAP_NET_BIND_SERVICE)].permitted |=
      CAP_TO_MASK(CAP_NET_BIND_SERVICE);
  cap[CAP_TO_INDEX(CAP_SYS_RAWIO)].permitted |= CAP_TO_MASK(CAP_SYS_RAWIO);
  cap[CAP_TO_INDEX(CAP_SYS_NICE)].permitted |= CAP_TO_MASK(CAP_SYS_NICE);
  cap[CAP_TO_INDEX(CAP_SETGID)].permitted |= CAP_TO_MASK(CAP_SETGID);
  cap[CAP_TO_INDEX(CAP_WAKE_ALARM)].permitted |= CAP_TO_MASK(CAP_WAKE_ALARM);

  cap[CAP_TO_INDEX(CAP_NET_RAW)].effective |= CAP_TO_MASK(CAP_NET_RAW);
  cap[CAP_TO_INDEX(CAP_NET_ADMIN)].effective |= CAP_TO_MASK(CAP_NET_ADMIN);
  cap[CAP_TO_INDEX(CAP_NET_BIND_SERVICE)].effective |=
      CAP_TO_MASK(CAP_NET_BIND_SERVICE);
  cap[CAP_TO_INDEX(CAP_SYS_RAWIO)].effective |= CAP_TO_MASK(CAP_SYS_RAWIO);
  cap[CAP_TO_INDEX(CAP_SYS_NICE)].effective |= CAP_TO_MASK(CAP_SYS_NICE);
  cap[CAP_TO_INDEX(CAP_SETGID)].effective |= CAP_TO_MASK(CAP_SETGID);
  cap[CAP_TO_INDEX(CAP_WAKE_ALARM)].effective |= CAP_TO_MASK(CAP_WAKE_ALARM);

  capset(&header, &cap[0]);
  setgroups(sizeof(groups) / sizeof(groups[0]), groups);
}

/*****************************************************************************
**   Logger API
*****************************************************************************/

/*******************************************************************************
 ** Console helper functions
 *******************************************************************************/

void skip_blanks(char** p) {
  while (**p == ' ') (*p)++;
}

#define is_cmd(str)                \
  ((strlen(str) == strlen(cmd)) && \
   strncmp((const char*)&cmd, str, strlen(str)) == 0)
#define if_cmd(str) if (is_cmd(str))

typedef void(t_console_cmd_handler)(char* p);

typedef struct {
  const char* name;
  t_console_cmd_handler* handler;
  const char* help;
  unsigned char is_job;
} t_cmd;

#if 0
static void cmdjob_handler(void *param)
{
    char *job_cmd = (char*)param;

   // process_cmd(job_cmd, 1);
    free(job_cmd);
}

static int create_cmdjob(char *cmd)
{
    pthread_t thread_id;
    char *job_cmd;

    job_cmd = malloc(strlen(cmd)+1); /* freed in job handler */
    if (job_cmd) {
        strlcpy(job_cmd, cmd, sizeof(job_cmd));

        if (pthread_create(&thread_id, NULL,
                       (void*)cmdjob_handler, (void*)job_cmd)!=0)
            perror("pthread_create");
    }

    return 0;
}
#endif

/*******************************************************************************
 ** Load stack lib
 *******************************************************************************/
#define BLUETOOTH_LIBRARY_NAME \
  "/apex/com.android.btservices/lib64/libbluetooth_jni.so"
int load_bt_lib(const bt_interface_t** interface) {
  const char* sym = BLUETOOTH_INTERFACE_STRING;
  bt_interface_t* itf = nullptr;

  // Always try to load the default Bluetooth stack on GN builds.
  const char* path = BLUETOOTH_LIBRARY_NAME;

  ABinderProcess_startThreadPool();  // start a binder thread to recieve
                                     // callback
  void* handle = dlopen(path, RTLD_NOW);
  if (!handle) {
    const char* err_str = dlerror();
    printf("failed to load Bluetooth library\n");
    goto error;
  }

  // Get the address of the bt_interface_t.
  itf = (bt_interface_t*)dlsym(handle, sym);
  if (!itf) {
    printf("failed to load symbol from Bluetooth library\n");
    goto error;
  }

  // Success.
  printf(" loaded HAL Success\n");
  *interface = itf;
  return 0;

error:
  *interface = NULL;
  if (handle) dlclose(handle);

  return -EINVAL;
}

int HAL_load(void) {
  if (load_bt_lib((bt_interface_t const**)&sBtInterface)) {
    printf("No Bluetooth Library found\n");
    return -1;
  }
  return 0;
}

int HAL_unload(void) {
  int err = 0;

  sBtInterface = NULL;

  return err;
}

/*******************************************************************************
 ** HAL test functions & callbacks
 *******************************************************************************/
#if 0
void setup_test_env(void)
{
    int i = 0;

    while (console_cmd_list[i].name != NULL)
    {
        console_cmd_maxlen = MAX(console_cmd_maxlen, (int)strlen(console_cmd_list[i].name));
        i++;
    }
}
#endif

void check_return_status(bt_status_t status) {
  if (status != BT_STATUS_SUCCESS) {
    printf("HAL REQUEST FAILED\n");
  } else {
    printf("\nHAL REQUEST SUCCESS");
  }
}

static void adapter_state_changed(bt_state_t state) {
  int V1 = 1000, V2 = 2;
  bt_property_t property = {BT_PROPERTY_ADAPTER_DISCOVERABLE_TIMEOUT, 4, &V1};
  bt_property_t property1 = {BT_PROPERTY_ADAPTER_SCAN_MODE /*SCAN*/, 2, &V2};
  bt_property_t property2 = {BT_PROPERTY_BDNAME, 6, (void*)"Amith"};
  g_AdapterState = state;

  if (state == BT_STATE_ON) {
    status = (bt_status_t)sBtInterface->set_adapter_property(&property1);
    status = (bt_status_t)sBtInterface->set_adapter_property(&property);
    status = (bt_status_t)sBtInterface->set_adapter_property(&property2);
  }
}

static void adapter_properties_changed(bt_status_t status, int num_properties,
                                       bt_property_t* properties) {
  char Bd_addr[15] = {0};
  if (NULL == properties) {
    return;
  }
  switch (properties->type) {
    case BT_PROPERTY_BDADDR:
      memcpy(Bd_addr, properties->val, properties->len);
      break;
    default:
      break;
  }
  return;
}

static void discovery_state_changed(bt_discovery_state_t state) {
  printf("Discovery State Updated : %s\n",
         (state == BT_DISCOVERY_STOPPED) ? "STOPPED" : "STARTED");
}

#if 0
static void pin_request_cb(RawAddress *remote_bd_addr, bt_bdname_t *bd_name, uint32_t cod)
{

    int ret = 0;
    bt_pin_code_t pincode = {{ 0x31, 0x32, 0x33, 0x34}};

    if(BT_STATUS_SUCCESS != sBtInterface->pin_reply(remote_bd_addr, TRUE, 4, &pincode)) {
        printf("Pin Reply failed\n");
    }
}
#endif
static void ssp_request_cb(RawAddress* remote_bd_addr,
                           bt_ssp_variant_t pairing_variant,
                           uint32_t pass_key) {
  if (BT_STATUS_SUCCESS != sBtInterface->ssp_reply(remote_bd_addr,
                                                   pairing_variant, TRUE,
                                                   pass_key)) {
    printf("SSP Reply failed\n");
  }
}

static void bond_state_changed_cb(bt_status_t status,
                                  RawAddress* remote_bd_addr,
                                  bt_bond_state_t state, int fail_reason) {
  g_PairState = state;
}

static void acl_state_changed(bt_status_t status, RawAddress* remote_bd_addr,
                              bt_acl_state_t state, int transport_link_type,
                              bt_hci_error_code_t hci_reason,
                              bt_conn_direction_t direction,
                              uint16_t acl_handle) {}

static bt_callbacks_t bt_callbacks = {
    sizeof(bt_callbacks_t),
    adapter_state_changed,
    adapter_properties_changed, /*adapter_properties_cb */
    NULL,                       /* remote_device_properties_cb */
    NULL,                       /* device_found_cb */
    discovery_state_changed,    /* discovery_state_changed_cb */
    NULL,                       /* pin_request_cb  */
    ssp_request_cb,             /* ssp_request_cb  */
    bond_state_changed_cb,      /*bond_state_changed_cb */
    NULL,                       /* address_consolidate_cb */
    NULL,
    acl_state_changed, /* acl_state_changed_cb */
    NULL,              /* thread_evt_cb */
    NULL,              /*energy_info_cb */
    NULL,              /* link_quality_report_cb */
    NULL,              /* generate_local_oob_data_cb */
    NULL,              /* switch_buffer_size_cb */
    NULL,              /* switch_codec_cb */
    NULL};

static int acquire_wake_lock(const char* lock_name) {
  return BT_STATUS_SUCCESS;
}

static int release_wake_lock(const char* lock_name) {
  return BT_STATUS_SUCCESS;
}

static bt_os_callouts_t callouts = {
    sizeof(bt_os_callouts_t),
    acquire_wake_lock,
    release_wake_lock,
};

void bdt_init(void) {
  printf("INIT BT \n");
  status = (bt_status_t)sBtInterface->init(&bt_callbacks, false, false, 0,
                                           nullptr, false, NULL);
  if (status == BT_STATUS_SUCCESS) {
    // Get Vendor Interface
    btvendorInterface =
        (btvendor_interface_t*)sBtInterface->get_profile_interface(
            BT_PROFILE_VENDOR_ID);
    if (!btvendorInterface) {
      printf("Error in loading vendor interface \n");
      exit(0);
    }
    status = (bt_status_t)sBtInterface->set_os_callouts(&callouts);
  }
  check_return_status(status);
}

void bdt_enable(void) {
  printf("ENABLE BT\n");
  if (BT_STATE_ON == g_AdapterState) {
    printf("Bluetooth is already enabled\n");
    return;
  }
  status = (bt_status_t)sBtInterface->enable();
  return;
}

void bdt_disable(void) {
  if (BT_STATE_ON != g_AdapterState) {
    return;
  }
  status = (bt_status_t)sBtInterface->disable();
  check_return_status(status);
  return;
}

void bdt_cleanup(void) { sBtInterface->cleanup(); }

btl2cap_interface_t* get_l2cap_interface(void) {
  if ((btvendorInterface) && sBtInterface) {
    return (btl2cap_interface_t*)btvendorInterface->get_testapp_interface(
        TEST_APP_L2CAP);
  }
  return NULL;
}

/*******************************************************************************
 ** Console commands
 *******************************************************************************/

void do_quit(char* p) { bdt_shutdown(); }

/*******************************************************************
 *
 *  BT TEST  CONSOLE COMMANDS
 *
 *  Parses argument lists and passes to API test function
 *
 */

void do_init(char* p) { bdt_init(); }

bool do_enable(char* p) {
  bdt_enable();
  if (0 != WaitForCompletion(BT_TURNON_CMD, 10)) {
    printf("BT Turn ON Failed... Exiting...\n");
    return FALSE;
  }
  return TRUE;
}

bool do_disable(char* p) {
  bdt_disable();
  if (0 != WaitForCompletion(BT_TURNOFF_CMD, 10)) {
    printf("BT Turn OFF Failed... Exiting...\n");
    return FALSE;
  }
  return TRUE;
}

void do_cleanup(char* p) {
  // bdt_cleanup();
}

void do_l2cap_init(char* p) {
  memset(&tl2cap_cfg_info, 0, sizeof(tl2cap_cfg_info));
  // Use macros for the constants
  tl2cap_cfg_info.mtu_present = TRUE;
  tl2cap_cfg_info.mtu = g_imtu;
  tl2cap_cfg_info.flush_to_present = TRUE;
  tl2cap_cfg_info.flush_to = 0xffff;
  // use other param if needed
  tl2cap_cfg_info.fcr_present = g_Fcr_Present;
  tl2cap_cfg_info.fcr.mode = g_Fcr_Mode;
  tl2cap_cfg_info.fcs = 0;
  tl2cap_cfg_info.fcs_present = 1;

  tl2cap_cfg_info.fcr.tx_win_sz = 3;
  if (L2CAP_FCR_ERTM_MODE == tl2cap_cfg_info.fcr.mode) {
    tl2cap_cfg_info.fcr = ertm_fcr_opts_def;
  } else if (L2CAP_FCR_STREAM_MODE == tl2cap_cfg_info.fcr.mode) {
    tl2cap_cfg_info.fcr = stream_fcr_opts_def;
  }
  // Initialize ERTM Parameters
  t_ertm_info.preferred_mode = g_Fcr_Mode;
#if 0
    t_ertm_info.allowed_modes = g_Ertm_AllowedMode;
    t_ertm_info.user_rx_buf_size  = BT_DEFAULT_BUFFER_SIZE;
    t_ertm_info.user_tx_buf_size = BT_DEFAULT_BUFFER_SIZE;
    t_ertm_info.fcr_rx_buf_size = BT_DEFAULT_BUFFER_SIZE;
    t_ertm_info.fcr_tx_buf_size = BT_DEFAULT_BUFFER_SIZE;
#endif
  // Load L2cap Interface
  if (NULL == sL2capInterface) {
    sL2capInterface = get_l2cap_interface();
  }
  if (sL2capInterface) sL2capInterface->Init(&l2test_l2c_appl);
}

void do_l2cap_deregister(char* p) { sL2capInterface->Deregister(g_PSM); }

uint16_t do_l2cap_connect(char* p) {
  RawAddress bd_addr;
  RawAddress::FromString(p, bd_addr);

  if ((L2CAP_FCR_STREAM_MODE == g_Fcr_Mode) ||
      (L2CAP_FCR_ERTM_MODE == g_Fcr_Mode)) {
    return sL2capInterface->ErtmConnectReq(g_PSM, bd_addr, &t_ertm_info);
  } else {
    return sL2capInterface->Connect(g_PSM, &bd_addr);
  }
}

bool do_l2cap_ping(char* p) {
  RawAddress bd_addr;
  RawAddress::FromString(p, bd_addr);
  if (FALSE == sL2capInterface->Ping(bd_addr, l2c_echo_rsp_cb)) {
    printf("Failed to send Ping Request \n");
    return FALSE;
  }
  return TRUE;
}

bool do_l2cap_disconnect(char* p) {
  return sL2capInterface->DisconnectReq(g_lcid);
}

uint8_t do_l2cap_DataWrite(char* p, uint32_t len) {
  return sL2capInterface->DataWrite(g_lcid, p, len);
}

void do_l2cap_SetSecConnOnlyMode(bool secvalue) {
  sL2capInterface->SetSecConnOnlyMode(secvalue);
}
static int WaitForCompletion(int Cmd, int Timeout) {
  int Status = 0xFF;
  int* pState = NULL;
  switch (Cmd) {
    case BT_TURNON_CMD:
      Status = BT_STATE_ON;
      pState = &g_AdapterState;
      break;
    case BT_TURNOFF_CMD:
      Status = BT_STATE_OFF;
      pState = &g_AdapterState;
      break;
    case I_CONNECT_CMD:
      Status = CONNECT;
      pState = &g_ConnectionState;
      break;
    case O_CONNECT_CMD:
      Status = CONNECT;
      pState = &g_ConnectionState;
      break;
    case DISCONNECT_CMD:
      Status = DISCONNECT;
      pState = &g_ConnectionState;
      break;
    case SEND_DATA_CMD:
      //
      break;
    case O_PAIR_CMD:
      Status = BT_BOND_STATE_BONDED;
      pState = &g_PairState;
      break;
  }
  if (NULL == pState) return 0xFF;
  while ((Status != *pState) && (Timeout--)) {
    sleep(1);
  }
  if (Status != *pState)
    return 1;  // Timeout
  else
    return 0;  // Success
}

static void l2c_listen(int SendData) {
  printf("Waiting for Incoming connection... \n");
  if (0 != WaitForCompletion(I_CONNECT_CMD, 60)) {
    printf("No incoming connection... Exiting...\n");
    return;
  }
  if (TRUE == SendData) {
    printf(" going to send data...\n");
    Send_Data();
  }
}

static int Send_Data() {
  int fd, size;
  char* tmpBuf = NULL;

  if (data_size < 0) data_size = g_omtu;
  printf("data_size = %ld, g_omtu=%d", data_size, g_omtu);

  tmpBuf = (char*)malloc(data_size);
  if (NULL == tmpBuf) {
    printf("Malloc failed \n");
    return FALSE;
  }
  if (filename) {
    fd = open(filename, O_RDONLY);
    printf("Filename for input data = %s \n", filename);
    if (fd < 0) {
      printf("Open failed: %s (%d)\n", strerror(errno), errno);
      exit(1);
    }
    while (1) {
      size = read(fd, tmpBuf, data_size);
      if (size <= 0) {
        printf("\n File end ");
        break;
      }
      do_l2cap_DataWrite(tmpBuf, size);
    }
    return TRUE;
  } else {
    memset(tmpBuf, '\x7f', data_size);
  }
  if (num_frames && g_delay && count) {
    printf("Delay before first send ... %lu msec, size=%ld \n", g_delay / 1000,
           data_size);
    usleep(g_delay);
  }
  printf(" count %d...\n", count);
  sleep(5);
  while (count > 0) {
    char tmpBuffer[] = {
        0x11, 0x04, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F,
        0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F,
        0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F,
        0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F,
        0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F,
        0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F,
        0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F,
        0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F,
        0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F,
        0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F,
        0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F,
        0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F,
        0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F,
        0x7F};
    count--;
    printf("Before write count is %d...\n", count);
    if (g_Sar_Present == TRUE) {
      sleep(5);
      printf("SAR present..\n");
      do_l2cap_DataWrite(tmpBuffer, 100);
      sleep(5);
      do_l2cap_DataWrite(tmpBuffer, 100);
    } else
      do_l2cap_DataWrite(tmpBuffer, 5);
  }
  sleep(50);
  free(tmpBuf);
  return TRUE;
}

static void l2c_connect(char* svr) {
  printf("In l2c_connect - %s \n", svr);
  do_l2cap_connect(svr);
}

static void l2c_send(char* p) {
  do_l2cap_connect(p);
  if (0 != WaitForCompletion(I_CONNECT_CMD, 10)) {
    printf("Connection didnot happen in 10sec... Returning Failure...\n");
    return;
  }
  sleep(1);  // Let Config to complete
  Send_Data();
}

static int l2c_pair(char* p) {
  RawAddress bd_addr;
  RawAddress::FromString(p, bd_addr);
  if (BT_STATUS_SUCCESS !=
      sBtInterface->create_bond(&bd_addr, TRANSPORT_BREDR)) {
    printf("Failed to Initiate Pairing \n");
    return FALSE;
  }
  if (0 != WaitForCompletion(O_PAIR_CMD, 15)) {
    printf("Pairing didnot happen in 15sec... Returning Failure...\n");
    return FALSE;
  }
  return TRUE;
}

static void l2c_ping(char* svr) {
  printf("In l2c_ping - %s \n", svr);
  do_l2cap_ping(svr);
}

static void l2c_disconnect(char* p) {
  printf("In l2c_disconnect\n");
  do_l2cap_disconnect(p);
}

static void options(void) {
  printf(
      "Modes:\n"
      "\t-c connect\n"
      "\t-r receive\n"
      "\t-s connect and send\n"
      "\t-w wait and send\n"
      "\t-p bonding\n"
      "\t-a ping\n");
  printf(
      "Options:\n"
      "\t[-b bytes] [-i device] [-P psm] [-J cid]\n"
      "\t[-I imtu] [-O omtu]\n"
      "\t[-L localBusy status] 1-localbusy, 0-otherwise (default=0)\n"
      "\t[-N num] send num frames (default = infinite)\n"
      "\t[-C num] Count(default = 1)\n"
      "\t[-D milliseconds] delay after sending num frames (default = 0)\n"
      "\t[-X mode] select retransmission/flow-control mode\n"
      "\t(ertm, ertm-mandatory, streaming, streaming-mandatory)\n"
      "\t[-Q num] retransmit each packet up to num times (default = 3)\n"
      "\t[-A] authentication\n"
      "\t[-E] encryption\n"
      "\t[-S] secure connection\n"
      "\t[-T] timestamps\n"
      "\t[-H mode] 1-SecConnOnlyModeTrue 0-SecConnOnlyModeFalse\n");
}

static int len = 0;

int main(int argc, char* argv[]) {
  struct sigaction sa;
  int opt, mode = RECEIVE, addr_required = 0;
  char temp[3] = {0};

  while ((opt = getopt(argc, argv,
                       "arswcpb:i:P:K:O:H:F:N:L:C:D:X:Q:I:W:Z:UGATMES")) !=
         EOF) {
    switch (opt) {
      case 'a':
        mode = PING;
        addr_required = 1;
        break;
      case 'r':
        mode = RECEIVE;
        g_ConnType = FALSE;
        break;

      case 's':
        mode = SEND;
        g_ConnType = TRUE;
        addr_required = 1;
        break;

      case 'w':
        mode = WAITANDSEND;
        g_ConnType = FALSE;
        break;

      case 'c':
        mode = CONNECT;
        g_ConnType = TRUE;
        addr_required = 1;
        break;

      case 'p':
        mode = PAIR;
        addr_required = 1;
        break;

      case 'b':
        data_size = atoi(optarg);
        break;

      case 'A':
        auth = 1;
        break;
      case 'C':
        count = atoi(optarg);
        break;
      case 'D':
        g_delay = atoi(optarg) * 1000;
        break;
      case 'E':
        encrypt = 1;
        break;
      case 'F':
        filename = strdup(optarg);
        break;
      case 'H':
        g_SecOnlyMode = TRUE;
        g_secvalue = atoi(optarg);
        break;
      case 'I':
        g_imtu = atoi(optarg);
        break;

      case 'L':
        g_LocalBusy = atoi(optarg);
        break;

      case 'M':
        master = 1;
        break;

      case 'N':
        num_frames = atoi(optarg);
        break;

      case 'O':
        g_omtu = atoi(optarg);
        break;

      case 'P':
        g_PSM = atoi(optarg);
        printf("PSM %d", g_PSM);
        break;
      case 'S':
        secure_m4_l4 = 1;
        break;

      case 'Q':
        ertm_fcr_opts_def.max_transmit = atoi(optarg);
        stream_fcr_opts_def.max_transmit = ertm_fcr_opts_def.max_transmit;
        break;

      case 'X':
        if (strcasecmp(optarg, "ertm-mandatory") == 0) {
          g_Fcr_Present = TRUE;
          g_Fcr_Mode = L2CAP_FCR_ERTM_MODE;
          g_Ertm_AllowedMode = L2CAP_FCR_CHAN_OPT_ERTM;
          printf("in ERTM Mandatory option - 1\n");
        } else if (strcasecmp(optarg, "ertm") == 0) {
          g_Fcr_Present = TRUE;
          g_Fcr_Mode = L2CAP_FCR_ERTM_MODE;
          printf("in ERTM option \n");
        } else if (strcasecmp(optarg, "streaming-mandatory") == 0) {
          printf("FCR Mode selected as Streaming Mandatory\n");
          g_Fcr_Present = TRUE;
          g_Fcr_Mode = L2CAP_FCR_STREAM_MODE;
          g_Ertm_AllowedMode = L2CAP_FCR_CHAN_OPT_STREAM;
        } else if (strcasecmp(optarg, "streaming") == 0) {
          g_Fcr_Present = TRUE;
          printf("FCR Mode selected as Streaming\n");
          g_Fcr_Mode = L2CAP_FCR_STREAM_MODE;
        } else {
          g_Fcr_Mode = L2CAP_FCR_BASIC_MODE;
          printf("FCR Mode selected as Basic. String passed matches none\n");
        }
        break;

      case 'W':
        ertm_fcr_opts_def.tx_win_sz = atoi(optarg);
        stream_fcr_opts_def.tx_win_sz = ertm_fcr_opts_def.tx_win_sz;
        tl2cap_cfg_info.fcr.tx_win_sz = ertm_fcr_opts_def.tx_win_sz;
        break;
      case 'Z':
        g_Sar_Present = TRUE;
        printf("g_Sar_Present = true \n");
        break;
      default:
        options();
        exit(1);
    }
  }
  if (addr_required && !(argc - optind)) {
    options();
    exit(1);
  }

  if (data_size < 0)
    buffer_size = (g_omtu > g_imtu) ? g_omtu : g_imtu;
  else
    buffer_size = data_size;

  if (!(buf = (unsigned char*)malloc(buffer_size))) {
    perror("Can't allocate data buffer");
    exit(1);
  }

  memset(&sa, 0, sizeof(sa));
  sa.sa_handler = SIG_IGN;
  sa.sa_flags = SA_NOCLDSTOP;
  sigaction(SIGCHLD, &sa, NULL);

  config_permissions();
  if (HAL_load() < 0) {
    perror("HAL failed to initialize, exit\n");
    unlink(PID_FILE);
    exit(0);
  }

  // setup_test_env();
  bdt_init();
  sleep(5);
  if (FALSE == do_enable(NULL)) goto ERR;
  printf("\n Before l2cap init\n");
  do_l2cap_init(NULL);
  printf("\n after l2cap init\n");
  if (g_SecOnlyMode) {
    printf("\n Before Setting SecureConnectionsOnlyMode");
    do_l2cap_SetSecConnOnlyMode(TRUE);
    printf("\n After Setting SecureConnectionsOnlyMode");
  }
  // Outgoing Connection
  if (TRUE == g_ConnType) {
    if (1 == auth) g_SecLevel |= BTM_SEC_OUT_AUTHENTICATE;
    if (1 == encrypt) g_SecLevel |= BTM_SEC_OUT_ENCRYPT;
    if (1 == secure_m4_l4) g_SecLevel |= BTM_SEC_MODE4_LEVEL4;
  } else {
    if (1 == auth) g_SecLevel |= BTM_SEC_IN_AUTHENTICATE;
    if (1 == encrypt) g_SecLevel |= BTM_SEC_IN_ENCRYPT;
    if (1 == secure_m4_l4) g_SecLevel |= BTM_SEC_MODE4_LEVEL4;
  }

  if (0 != g_PSM) {
    printf("g_SecLevel = %d \n", g_SecLevel);
    sL2capInterface->RegisterPsm(g_PSM, g_ConnType, &t_ertm_info,
                                 L2CAP_DEFAULT_MTU, g_omtu, g_SecLevel);
    sleep(3);
  }

  switch (mode) {
    case RECEIVE:
      l2c_listen(FALSE);
      break;

    case SEND:
      l2c_send(argv[optind]);
      break;

    case WAITANDSEND:
      l2c_listen(TRUE);
      break;

    case CONNECT:
      l2c_connect(argv[optind]);
      break;

    case PING:
      l2c_ping(argv[optind]);
      break;

    case PAIR:
      l2c_pair(argv[optind]);
      if (0 != g_PSM) {
        sleep(2);
        l2c_connect(argv[optind]);
      }
      break;
  }

  if (0 != g_LocalBusy) {
    sleep(5);
    printf("To Send Local BusyStatus.... Press any key\n");
    read(0, &temp, 2);
    sL2capInterface->FlowControl(
        g_lcid,
        0);  // second param is 'dataEnabled', making it false means localBusy
  }

ERR:
  while (1) {
    sleep(5);
    printf("Enter Y/y to Exit... \n");
    len = read(0, &temp, 2);
    if ((temp[0] == 'Y') || (temp[0] == 'y')) break;
  }
  // bdt_cleanup();
  if (g_ConnectionState == CONNECT) {
    l2c_disconnect(NULL);
    sleep(5);
  }
  do_disable(NULL);
  HAL_unload();
  return 0;
}
