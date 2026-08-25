#pragma once

#include <QString>

/** @brief 一条对话消息的说话方。 */
enum class AiChatRole {
    System,
    User,
    Assistant
};

/** @brief 单条对话消息，请求体按 OpenAI 兼容 Chat Completions 的 messages 数组拼装。 */
struct AiChatMessage {
    AiChatRole role = AiChatRole::User;
    QString content;
};
