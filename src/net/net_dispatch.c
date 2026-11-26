/**
 * net_dispatch.c —— 网络请求调度（纯 C，无 Arduino 依赖）
 *
 * 只做一件事：按 `net_request_t.op` 选端点、传参数，**不碰协议细节**。
 * 明文组装、`edu_stage` 注入、加签、加密、验签、解密全在 net_api 层完成
 * （doc/05 §5.1：net 层是注入与加密的唯一职责，UI 不感知、也不得自行拼装）。
 *
 * 纯 C 的意义：主机侧可以给它喂桩实现，验证「op 是否路由到正确的端点」，
 * 不必上真机（见 test/test_net_dispatch.c）。
 */

#include "net_api.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "net_result.h"

int net_dispatch(const net_request_t *req, char **out_resp_json)
{
    if (req == NULL) {
        return -1;
    }
    int biz = -1;
    char *resp = NULL;
    bool ok = false;

    switch (req->op) {
    case NET_OP_SOLVE:
        /* 当前卡片上下文随请求带上来（doc/05 §5.1）——
         * 服务端据此把学生屏幕上那张卡的内容注入拍题教学 prompt。 */
        ok = net_study_solve(req->session_id, req->pack_id, req->card_id,
                             req->jpg, (size_t)req->jpg_len,
                             (const uint8_t *)req->pcm, (size_t)req->pcm_bytes,
                             req->hint_level, &biz, &resp);
        /*
         * 不排队、不补传（2026-11-26 定）：
         *   传输失败（biz<0）-> UI 提示「目前无网络」，用户重拍即可；
         *   云端暂时性故障（4290/5001）-> 已有专门提示，用户重试即可。
         * 照片一直只在内存里活到这次请求结束（所有权随 net_request_t 转给网络任务）。
         */
        break;
    case NET_OP_VOICE:
        ok = net_study_voice(req->context_page, req->pack_id, req->card_id,
                             (const uint8_t *)req->pcm, (size_t)req->pcm_bytes,
                             &biz, &resp);
        /* 同上：不排队、不补传，录音用完即释放，用户重录一次即可。 */
        break;
    case NET_OP_BOOTSTRAP:
        ok = net_sync_bootstrap_auto(&biz, &resp);
        if (ok && resp != NULL) {
            /* 心跳应答里的待下载清单：拉取并落盘（课表 / 学科包 / 电子书） */
            net_sync_pending_assets(resp, strlen(resp));
        } else {
            /*
             * 心跳没通（断网 / 未配对）：给 UI 一份**带 offline 标记的空应答**。
             * UI 据此切到 TF 卡上的本地快照兜底（而不是把空数据当新数据）。
             * 标记是固件内部的约定，服务端不会返回这个字段。
             */
            free(resp);
            resp = (char *)malloc(32);
            if (resp != NULL) {
                snprintf(resp, 32, "{\"weather\":{},\"offline\":true}");
            }
            biz = 0;
        }
        break;
    case NET_OP_CITY:
        /* 设置城市：写进 net 层缓存，下一次心跳明文里带上（doc/05 §5.2）。
         * 真正的落盘（TF 卡）在 UI 侧做 —— net 任务不得直接扫盘。 */
        net_set_city(req->city);
        ok = true;
        biz = 0;
        break;
    case NET_OP_PAIR:
        ok = net_pair_with_code(req->pair_code);
        biz = ok ? 0 : -1;
        break;
    case NET_OP_REGISTER:
        /* 自动登记：设备自报身份，网关幂等签发密钥（doc/04 §1.2） */
        ok = net_auto_register();
        biz = ok ? 0 : -1;
        break;
    case NET_OP_PROFILE:
        /* 档案同步不是一次网络请求，只是把档案灌进 net 层缓存，
         * 由 net 层在组装明文时自动注入（doc/05 §5.1）。 */
        ok = net_profile_set(req->edu_stage, req->enrollment_year, req->remaining_quota);
        biz = ok ? 0 : -1;
        break;
    default:
        return -1;
    }

    if (out_resp_json != NULL) {
        *out_resp_json = resp;   /* 所有权转移给调用方 */
    } else {
        free(resp);
    }
    return ok ? biz : -1;
}
