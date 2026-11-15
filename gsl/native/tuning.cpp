/* SPDX-License-Identifier: BSD-3-Clause */
#include <ar_osal_error.h>
#include <gsl_dls_client_intf.h>
#include <gsl_rtc_intf.h>

extern "C" int32_t gsl_rtc_get_active_usecase_info(struct gsl_rtc_active_uc_info *)
{
    return AR_EUNSUPPORTED;
}

extern "C" int32_t gsl_rtc_get_persist_data(struct gsl_rtc_persist_param *)
{
    return AR_EUNSUPPORTED;
}

extern "C" int32_t gsl_rtc_get_non_persist_data(struct gsl_rtc_param *)
{
    return AR_EUNSUPPORTED;
}

extern "C" int32_t gsl_rtc_set_persist_data(struct gsl_rtc_persist_param *)
{
    return AR_EUNSUPPORTED;
}

extern "C" int32_t gsl_rtc_set_non_persist_data(struct gsl_rtc_param *)
{
    return AR_EUNSUPPORTED;
}

extern "C" int32_t gsl_rtc_prepare_change_graph(
    struct gsl_rtc_prepare_change_graph_info *)
{
    return AR_EUNSUPPORTED;
}

extern "C" int32_t gsl_rtc_change_graph(struct gsl_rtc_change_graph_info *)
{
    return AR_EUNSUPPORTED;
}

extern "C" int32_t gsl_rtc_notify_conn_state(struct gsl_rtc_conn_info *)
{
    return AR_EUNSUPPORTED;
}

extern "C" int32_t gsl_dls_client_is_feature_supported()
{
    return AR_EUNSUPPORTED;
}

extern "C" int32_t gsl_dls_client_init(struct gsl_dls_buffer_pool_config_t *,
                                       GSL_DLS_CLIENT_BUFFER_READY_CALLBACK)
{
    return AR_EUNSUPPORTED;
}

extern "C" int32_t gsl_dls_client_deinit()
{
    return AR_EUNSUPPORTED;
}

extern "C" int32_t gsl_dls_client_create_buffer_pool(
    struct gsl_dls_buffer_pool_config_t *)
{
    return AR_EUNSUPPORTED;
}

extern "C" void gsl_dls_client_get_ready_dls_buffer_list(
    struct gsl_dls_ready_buffer_index_list_t *)
{
}

extern "C" int32_t gsl_dls_client_get_log_buffer(uint32_t,
                                                 struct gls_dls_buffer_t *)
{
    return AR_EUNSUPPORTED;
}

extern "C" int32_t gsl_dls_client_return_used_buffers(
    struct gsl_dls_ready_buffer_index_list_t *)
{
    return AR_EUNSUPPORTED;
}

extern "C" int32_t gsl_dls_client_register_deregister_log_code(uint32_t, bool_t)
{
    return AR_EUNSUPPORTED;
}
