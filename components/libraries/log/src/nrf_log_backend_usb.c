/**
 * Copyright 2021 Evgeniy Morozov
 *
 * Redistribution and use in source and binary forms, with or without modification,
 * are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 * this list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 * this list of conditions and the following disclaimer in the documentation
 * and/or other materials provided with the distribution.
 *
 * 3. Neither the name of the copyright holder nor the names of its contributors
 * may be used to endorse or promote products derived from this software without
 * specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
 * STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY
 * WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE
*/

#include "sdk_common.h"

#if NRF_MODULE_ENABLED(NRF_LOG) && LOG_BACKEND_USB_ENABLED

#include "nrf_log_backend_usb.h"
#include "nrf_log_backend_serial.h"
#include "nrf_log_internal.h"
#include "nrf_log.h"
#include "app_error.h"

#include "nrfx_usbd.h"

#include "app_usbd.h"
#include "app_usbd_cdc_acm.h"

#if LOG_BACKEND_USB_INIT_STACK
#include "nrf_drv_clock.h"
#include "nrf_drv_power.h"
#include "app_timer.h"
#include "app_usbd_serial_num.h"
#endif /* LOG_BACKEND_USB_INIT_STACK */

static uint8_t gs_string_buffer[LOG_BACKEND_USB_TMP_BUFFER_SIZE];
static bool gs_initialized;
static volatile bool gs_tx_done;
static volatile bool gs_port_opened;

#if LOG_BACKEND_USB_INIT_STACK
static void log_backend_usbd_user_ev_handler(app_usbd_event_type_t event);
static const app_usbd_config_t gs_usbd_config = {
    .ev_state_proc = log_backend_usbd_user_ev_handler,
};
#endif /* LOG_BACKEND_USB_INIT_STACK */

static void log_backend_usb_cdc_acm_ev_handler(app_usbd_class_inst_t const * p_inst,
                                               app_usbd_cdc_acm_user_event_t event);
APP_USBD_CDC_ACM_GLOBAL_DEF(gs_log_usb_cdc_acm,
                            log_backend_usb_cdc_acm_ev_handler,
                            LOG_BACKEND_USB_CDC_ACM_COMM_INTERFACE,
                            LOG_BACKEND_USB_CDC_ACM_DATA_INTERFACE,
                            NRFX_USBD_EPIN(LOG_BACKEND_USB_CDC_ACM_COMM_EPIN),
                            NRFX_USBD_EPIN(LOG_BACKEND_USB_CDC_ACM_DATA_EPIN),
                            NRFX_USBD_EPOUT(LOG_BACKEND_USB_CDC_ACM_DATA_EPOUT),
                            APP_USBD_CDC_COMM_PROTOCOL_AT_V250
    );

void log_backend_usb_init(void)
{
    ret_code_t ret = NRF_SUCCESS;

    if (gs_initialized)
    {
        return;
    }

#if LOG_BACKEND_USB_INIT_STACK
    /* 1. Try to init the clock driver, skip if initialized */
    if (!nrf_drv_clock_init_check())
    {
        ret = nrf_drv_clock_init();
        APP_ERROR_CHECK(ret);
    }

    /* 2. Start up the Low Frequency clocking if it is not running already */
    if (!nrf_drv_clock_lfclk_is_running())
    {
        nrf_drv_clock_lfclk_request(NULL);
        while (!nrf_drv_clock_lfclk_is_running())
        {
            /* Just waiting until the lf clock starts up */
        }
    }

    app_usbd_serial_num_generate();

    /** 3. Init the USBD library. We assume that user will only init USB using this library
      * which may not be true. Well, in that case it feels like the user would probably want
      * to undefine the app_usbd_init.
      */
    ret = app_usbd_init(&gs_usbd_config);
    if (NRFX_ERROR_INVALID_STATE != ret)
    {
        APP_ERROR_CHECK(ret);
    }
#endif /* LOG_BACKEND_USB_INIT_STACK */

    /* Add the CDC ACM device class to the USBD after it was initialized */
    app_usbd_class_inst_t const * class_cdc_acm = app_usbd_cdc_acm_class_inst_get(&gs_log_usb_cdc_acm);
    ret = app_usbd_class_append(class_cdc_acm);
    APP_ERROR_CHECK(ret);

#if LOG_BACKEND_USB_INIT_STACK
#if LOG_BACKEND_USB_UTILIZE_POWER_EVENTS && APP_USBD_CONFIG_POWER_EVENTS_PROCESS
    ret = app_usbd_power_events_enable();
    APP_ERROR_CHECK(ret);
#else
    if (!nrf_drv_usbd_is_enabled())
    {
        app_usbd_enable();
    }
    if (!nrf_drv_usbd_is_started())
    {
        app_usbd_start();
    }
#endif /* APP_USBD_CONFIG_POWER_EVENTS_PROCESS */

    /* Wait until USBD starts up */
    while (!nrf_drv_usbd_is_started())
    {
        while (app_usbd_event_queue_process())
        {
        }
    }
#endif /* LOG_BACKEND_USB_INIT_STACK */

    gs_initialized = true;
}

#if LOG_BACKEND_USB_INIT_STACK
void log_backend_usb_process(void)
{
    if (!gs_initialized)
    {
        return;
    }

    while (app_usbd_event_queue_process())
    {
        /* Wait for USB events in the mean time */
    }
}
#endif /* LOG_BACKEND_USB_INIT_STACK */

#if LOG_BACKEND_USB_INIT_STACK
static void log_backend_usbd_user_ev_handler(app_usbd_event_type_t event)
{
    switch (event)
    {
    case APP_USBD_EVT_STOPPED:
        app_usbd_disable();
        break;
    case APP_USBD_EVT_POWER_DETECTED:
        if (!nrf_drv_usbd_is_enabled())
        {
            app_usbd_enable();
        }
        break;
    case APP_USBD_EVT_POWER_REMOVED:
        app_usbd_stop();
        break;
    case APP_USBD_EVT_POWER_READY:
        if (!nrf_drv_usbd_is_started())
        {
            app_usbd_start();
        }
        break;
    default:
        break;
    }
}
#endif /* LOG_BACKEND_USB_INIT_STACK */

static void log_backend_usb_cdc_acm_ev_handler(app_usbd_class_inst_t const * p_inst,
                                               app_usbd_cdc_acm_user_event_t event)
{
    switch (event)
    {
    case APP_USBD_CDC_ACM_USER_EVT_PORT_OPEN:
    {
        gs_port_opened = true;
        break;
    }
    case APP_USBD_CDC_ACM_USER_EVT_PORT_CLOSE:
    {
        gs_port_opened = false;
        break;
    }
    case APP_USBD_CDC_ACM_USER_EVT_TX_DONE:
    {
        gs_tx_done = true;
        break;
    }
    default:
        break;
    }
}

static void log_backend_usb_put(nrf_log_backend_t const * p_backend,
                                nrf_log_entry_t * p_msg);
static void log_backend_usb_flush(nrf_log_backend_t const * p_backend);
static void log_backend_usb_panic_set(nrf_log_backend_t const * p_backend);

static void usb_serial_tx(void const * p_context, char const * p_buffer, size_t len);

const nrf_log_backend_api_t log_backend_usb_api = {
    .put       = log_backend_usb_put,
    .flush     = log_backend_usb_flush,
    .panic_set = log_backend_usb_panic_set,
};

static void log_backend_usb_put(nrf_log_backend_t const * p_backend,
                                nrf_log_entry_t * p_msg)
{
    while (app_usbd_event_queue_process())
    {
        /* Nothing to do */
    }

    nrf_log_backend_serial_put(p_backend,
                               p_msg,
                               gs_string_buffer,
                               LOG_BACKEND_USB_TMP_BUFFER_SIZE,
                               usb_serial_tx);
}

static void log_backend_usb_flush(nrf_log_backend_t const * p_backend)
{
    /* No flush for USB that I can think of for now. */
}

static void log_backend_usb_panic_set(nrf_log_backend_t const * p_backend)
{
    /* We can't do anything without interrupts over the USB so we just gracefully exit by
       detaching the USB class */
    app_usbd_class_inst_t const * class_cdc_acm = app_usbd_cdc_acm_class_inst_get(&gs_log_usb_cdc_acm);
    ret_code_t ret = app_usbd_class_remove(class_cdc_acm);
    APP_ERROR_CHECK(ret);

    /* TODO: Deinit USB if we did that manually in the library */
}

static void usb_serial_tx(void const * p_context, char const * p_buffer, size_t len)
{
    if (!gs_port_opened)
    {
        return;
    }

    size_t offset = 0;
    do
    {
        size_t tx_len = len - offset;
        if (tx_len > NRFX_USBD_EPSIZE)
        {
            tx_len = NRFX_USBD_EPSIZE;
        }

        gs_tx_done = false;
        ret_code_t ret = app_usbd_cdc_acm_write(&gs_log_usb_cdc_acm,
                                                p_buffer + offset,
                                                tx_len);
        APP_ERROR_CHECK(ret);
        while (!gs_tx_done)
        {
            while (app_usbd_event_queue_process())
            {
                /* Wait until we're ready to send the data again */
            }
        }

        offset += tx_len;
    } while (offset < len);
}

#endif    /* NRF_MODULE_ENABLED(NRF_LOG) && defined(LOG_BACKEND_USB_ENABLED) */

