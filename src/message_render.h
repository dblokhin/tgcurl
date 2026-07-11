// Render a td_api::message into tgcurl's JSON message shape.
//
// Shared by every command that emits messages (`chat`, `search`) so the shape
// stays identical everywhere:
//   {id, [chat_id,] date, is_outgoing, sender_id, type, text,
//    reply_to_message_id}
// - `type` names the content ("text", "photo", "voice_note", ...) so an agent
//   knows *what* a non-text message is instead of guessing from an empty
//   string.
// - `text` is the message text for text messages and the caption for media;
//   for stickers it is the emoji, for polls the question. Empty when the
//   content carries no text at all.
// - `reply_to_message_id` is the replied-to message in the same chat, 0 when
//   the message is not a reply.
//
// Pure logic over td_api objects (no network) — unit-tested.
#ifndef TGCURL_MESSAGE_RENDER_H
#define TGCURL_MESSAGE_RENDER_H

#include "json_out.h"

#include <cstdint>
#include <string>
#include <td/telegram/td_api.h>

namespace tgcurl {

// The numeric id (user_id or chat_id) behind a MessageSender, or 0.
inline std::int64_t sender_id_of(const td::td_api::MessageSender* sender) {
    namespace td_api = td::td_api;
    if (sender == nullptr) {
        return 0;
    }
    switch (sender->get_id()) {
    case td_api::messageSenderUser::ID:
        // Safe downcast: get_id() matched.
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast)
        return static_cast<const td_api::messageSenderUser&>(*sender).user_id_;
    case td_api::messageSenderChat::ID:
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast)
        return static_cast<const td_api::messageSenderChat&>(*sender).chat_id_;
    default:
        return 0;
    }
}

// Stable content-type tag for a message's content.
inline std::string content_type_name(const td::td_api::MessageContent* content) {
    namespace td_api = td::td_api;
    if (content == nullptr) {
        return "other";
    }
    switch (content->get_id()) {
    case td_api::messageText::ID:
        return "text";
    case td_api::messagePhoto::ID:
        return "photo";
    case td_api::messageVideo::ID:
        return "video";
    case td_api::messageDocument::ID:
        return "document";
    case td_api::messageAudio::ID:
        return "audio";
    case td_api::messageAnimation::ID:
        return "animation";
    case td_api::messageVoiceNote::ID:
        return "voice_note";
    case td_api::messageVideoNote::ID:
        return "video_note";
    case td_api::messageSticker::ID:
        return "sticker";
    case td_api::messageAnimatedEmoji::ID:
        return "animated_emoji";
    case td_api::messageLocation::ID:
        return "location";
    case td_api::messageContact::ID:
        return "contact";
    case td_api::messagePoll::ID:
        return "poll";
    case td_api::messageCall::ID:
        return "call";
    case td_api::messageContactRegistered::ID:
        // Service notice: "<contact> joined Telegram".
        return "contact_registered";
    default:
        return "other";
    }
}

// True when the content is something a person (or bot) deliberately sent:
// text, media, contact, poll, dice, a call record and so on. False for
// service/system content — chat membership changes, pins, video-chat events,
// "X joined Telegram", payment/giveaway notices, expired media and any type
// this build doesn't know. A whitelist on purpose: TDLib grows a few new
// service types every release, and an unknown type polluting an agent's
// context is worse than a rare exotic user type being hidden (recoverable
// with `chat --all`).
inline bool is_user_message(const td::td_api::MessageContent* content) {
    namespace td_api = td::td_api;
    if (content == nullptr) {
        return false;
    }
    switch (content->get_id()) {
    case td_api::messageText::ID:
    case td_api::messagePhoto::ID:
    case td_api::messageVideo::ID:
    case td_api::messageDocument::ID:
    case td_api::messageAudio::ID:
    case td_api::messageAnimation::ID:
    case td_api::messageVoiceNote::ID:
    case td_api::messageVideoNote::ID:
    case td_api::messagePaidMedia::ID:
    case td_api::messageSticker::ID:
    case td_api::messageAnimatedEmoji::ID:
    case td_api::messageDice::ID:
    case td_api::messageGame::ID:
    case td_api::messageLocation::ID:
    case td_api::messageVenue::ID:
    case td_api::messageContact::ID:
    case td_api::messagePoll::ID:
    case td_api::messageStory::ID:
    case td_api::messageChecklist::ID:
    case td_api::messageInvoice::ID:
    case td_api::messageCall::ID:
        return true;
    default:
        return false;
    }
}

namespace detail {
inline std::string formatted(const td::td_api::object_ptr<td::td_api::formattedText>& text) {
    return text != nullptr ? text->text_ : "";
}
} // namespace detail

// The human text carried by the content: the text itself, a media caption,
// a sticker's emoji or a poll's question; "" when there is none.
inline std::string message_body(const td::td_api::MessageContent* content) {
    namespace td_api = td::td_api;
    if (content == nullptr) {
        return "";
    }
    switch (content->get_id()) {
    case td_api::messageText::ID:
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast)
        return detail::formatted(static_cast<const td_api::messageText&>(*content).text_);
    case td_api::messagePhoto::ID:
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast)
        return detail::formatted(static_cast<const td_api::messagePhoto&>(*content).caption_);
    case td_api::messageVideo::ID:
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast)
        return detail::formatted(static_cast<const td_api::messageVideo&>(*content).caption_);
    case td_api::messageDocument::ID:
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast)
        return detail::formatted(static_cast<const td_api::messageDocument&>(*content).caption_);
    case td_api::messageAudio::ID:
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast)
        return detail::formatted(static_cast<const td_api::messageAudio&>(*content).caption_);
    case td_api::messageAnimation::ID:
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast)
        return detail::formatted(static_cast<const td_api::messageAnimation&>(*content).caption_);
    case td_api::messageVoiceNote::ID:
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast)
        return detail::formatted(static_cast<const td_api::messageVoiceNote&>(*content).caption_);
    case td_api::messageSticker::ID: {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast)
        const auto& sticker = static_cast<const td_api::messageSticker&>(*content);
        return sticker.sticker_ != nullptr ? sticker.sticker_->emoji_ : "";
    }
    case td_api::messageAnimatedEmoji::ID:
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast)
        return static_cast<const td_api::messageAnimatedEmoji&>(*content).emoji_;
    case td_api::messagePoll::ID: {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast)
        const auto& poll = static_cast<const td_api::messagePoll&>(*content);
        return poll.poll_ != nullptr ? detail::formatted(poll.poll_->question_) : "";
    }
    default:
        return "";
    }
}

namespace detail {
// The file of the largest photo variant (the size vector is not guaranteed
// ordered), or nullptr.
inline const td::td_api::file* largest_photo_file(const td::td_api::photo* photo) {
    if (photo == nullptr) {
        return nullptr;
    }
    const td::td_api::file* best = nullptr;
    std::int64_t best_area = -1;
    for (const auto& size : photo->sizes_) {
        if (size == nullptr || size->photo_ == nullptr) {
            continue;
        }
        const std::int64_t area =
            static_cast<std::int64_t>(size->width_) * static_cast<std::int64_t>(size->height_);
        if (area > best_area) {
            best_area = area;
            best = size->photo_.get();
        }
    }
    return best;
}
} // namespace detail

// The downloadable file carried by the content — the document/video/audio
// itself, a photo's largest size, and so on — or nullptr when the content has
// no file (text, location, poll, ...). `name` receives Telegram's original
// file name when the media carries one ("" otherwise: photos, voice/video
// notes and stickers have no file name).
inline const td::td_api::file* content_file(const td::td_api::MessageContent* content,
                                            std::string& name) {
    namespace td_api = td::td_api;
    name.clear();
    if (content == nullptr) {
        return nullptr;
    }
    switch (content->get_id()) {
    case td_api::messagePhoto::ID:
        return detail::largest_photo_file(
            // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast)
            static_cast<const td_api::messagePhoto&>(*content).photo_.get());
    case td_api::messageVideo::ID: {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast)
        const auto& video = static_cast<const td_api::messageVideo&>(*content);
        if (video.video_ == nullptr) {
            return nullptr;
        }
        name = video.video_->file_name_;
        return video.video_->video_.get();
    }
    case td_api::messageDocument::ID: {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast)
        const auto& doc = static_cast<const td_api::messageDocument&>(*content);
        if (doc.document_ == nullptr) {
            return nullptr;
        }
        name = doc.document_->file_name_;
        return doc.document_->document_.get();
    }
    case td_api::messageAudio::ID: {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast)
        const auto& audio = static_cast<const td_api::messageAudio&>(*content);
        if (audio.audio_ == nullptr) {
            return nullptr;
        }
        name = audio.audio_->file_name_;
        return audio.audio_->audio_.get();
    }
    case td_api::messageAnimation::ID: {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast)
        const auto& anim = static_cast<const td_api::messageAnimation&>(*content);
        if (anim.animation_ == nullptr) {
            return nullptr;
        }
        name = anim.animation_->file_name_;
        return anim.animation_->animation_.get();
    }
    case td_api::messageVoiceNote::ID: {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast)
        const auto& voice = static_cast<const td_api::messageVoiceNote&>(*content);
        return voice.voice_note_ != nullptr ? voice.voice_note_->voice_.get() : nullptr;
    }
    case td_api::messageVideoNote::ID: {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast)
        const auto& note = static_cast<const td_api::messageVideoNote&>(*content);
        return note.video_note_ != nullptr ? note.video_note_->video_.get() : nullptr;
    }
    case td_api::messageSticker::ID: {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast)
        const auto& sticker = static_cast<const td_api::messageSticker&>(*content);
        return sticker.sticker_ != nullptr ? sticker.sticker_->sticker_.get() : nullptr;
    }
    default:
        return nullptr;
    }
}

// The same-chat message id this message replies to, or 0.
inline std::int64_t reply_to_message_id(const td::td_api::message& msg) {
    namespace td_api = td::td_api;
    if (msg.reply_to_ == nullptr || msg.reply_to_->get_id() != td_api::messageReplyToMessage::ID) {
        return 0;
    }
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast)
    const auto& reply = static_cast<const td_api::messageReplyToMessage&>(*msg.reply_to_);
    // Replies to another chat (channel discussion etc.) aren't addressable in
    // this chat; report those as "not a reply here".
    return reply.chat_id_ == msg.chat_id_ ? reply.message_id_ : 0;
}

// Serialize one message. with_chat_id adds the chat_id field — used by
// cross-chat outputs (global search); per-chat outputs omit it as redundant.
inline std::string message_json(const td::td_api::message& msg, bool with_chat_id = false) {
    json::Writer w;
    w.field("id", static_cast<std::int64_t>(msg.id_));
    if (with_chat_id) {
        w.field("chat_id", static_cast<std::int64_t>(msg.chat_id_));
    }
    w.field("date", msg.date_);
    w.field("is_outgoing", msg.is_outgoing_);
    w.field("sender_id", sender_id_of(msg.sender_id_.get()));
    w.field("type", content_type_name(msg.content_.get()));
    w.field("text", message_body(msg.content_.get()));
    w.field("reply_to_message_id", reply_to_message_id(msg));
    return w.object();
}

} // namespace tgcurl

#endif // TGCURL_MESSAGE_RENDER_H
