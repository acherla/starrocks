// This file is licensed under the Elastic License 2.0. Copyright 2021-present, StarRocks Limited.

#include "agent/report_task.h"

#include "agent/client_cache.h"
#include "agent/utils.h"
#include "gen_cpp/FrontendService_types.h"

namespace starrocks {

AgentStatus report_task(const TReportRequest& request, TMasterResult* result) {
    MasterServerClient client(&g_frontend_service_client_cache);
    return client.report(request, result);
}

} // namespace starrocks