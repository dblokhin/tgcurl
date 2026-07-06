// Tests for the shared message renderer (message_render.h): content-type
// tagging, text/caption extraction, reply detection and the JSON shape used
// by `chat` and `search`. Pure logic over constructed td_api objects.
#include "message_render.h"
#include "test_util.h"

#include <string>
#include <td/telegram/td_api.h>
#include <utility>

using namespace tgcurl;
namespace td_api = td::td_api;

namespace {

td_api::object_ptr<td_api::formattedText> ftext(const std::string& s) {
    auto t = td_api::make_object<td_api::formattedText>();
    t->text_ = s;
    return t;
}

td_api::object_ptr<td_api::message> base_message() {
    auto msg = td_api::make_object<td_api::message>();
    msg->id_ = 111;
    msg->chat_id_ = 42;
    msg->date_ = 1700000000;
    msg->is_outgoing_ = true;
    auto sender = td_api::make_object<td_api::messageSenderUser>();
    sender->user_id_ = 7;
    msg->sender_id_ = std::move(sender);
    return msg;
}

} // namespace

int main() {
    // Text message: type "text", body is the text.
    {
        auto msg = base_message();
        auto content = td_api::make_object<td_api::messageText>();
        content->text_ = ftext("hello");
        msg->content_ = std::move(content);

        CHECK_EQ(content_type_name(msg->content_.get()), "text");
        CHECK_EQ(message_body(msg->content_.get()), "hello");
        std::string json = message_json(*msg);
        CHECK(json.find("\"id\":111") != std::string::npos);
        CHECK(json.find("\"sender_id\":7") != std::string::npos);
        CHECK(json.find("\"type\":\"text\"") != std::string::npos);
        CHECK(json.find("\"text\":\"hello\"") != std::string::npos);
        CHECK(json.find("\"reply_to_message_id\":0") != std::string::npos);
        CHECK(json.find("\"chat_id\"") == std::string::npos); // per-chat shape
        // Cross-chat shape carries the chat_id.
        CHECK(message_json(*msg, /*with_chat_id=*/true).find("\"chat_id\":42") !=
              std::string::npos);
    }

    // Photo with caption: type "photo", body is the caption (was "" before —
    // the agent couldn't tell a photo from nothing).
    {
        auto content = td_api::make_object<td_api::messagePhoto>();
        content->caption_ = ftext("look at this");
        CHECK_EQ(content_type_name(content.get()), "photo");
        CHECK_EQ(message_body(content.get()), "look at this");
    }

    // Voice note without caption: typed, empty body.
    {
        auto content = td_api::make_object<td_api::messageVoiceNote>();
        CHECK_EQ(content_type_name(content.get()), "voice_note");
        CHECK_EQ(message_body(content.get()), "");
    }

    // Sticker: body is the emoji.
    {
        auto content = td_api::make_object<td_api::messageSticker>();
        content->sticker_ = td_api::make_object<td_api::sticker>();
        content->sticker_->emoji_ = "👍";
        CHECK_EQ(content_type_name(content.get()), "sticker");
        CHECK_EQ(message_body(content.get()), "👍");
    }

    // Animated emoji: body is the emoji itself.
    {
        auto content = td_api::make_object<td_api::messageAnimatedEmoji>();
        content->emoji_ = "❤️";
        CHECK_EQ(content_type_name(content.get()), "animated_emoji");
        CHECK_EQ(message_body(content.get()), "❤️");
    }

    // Poll: body is the question.
    {
        auto content = td_api::make_object<td_api::messagePoll>();
        content->poll_ = td_api::make_object<td_api::poll>();
        content->poll_->question_ = ftext("lunch?");
        CHECK_EQ(content_type_name(content.get()), "poll");
        CHECK_EQ(message_body(content.get()), "lunch?");
    }

    // Unknown/absent content degrades to "other"/"".
    {
        CHECK_EQ(content_type_name(nullptr), "other");
        CHECK_EQ(message_body(nullptr), "");
        auto content = td_api::make_object<td_api::messageExpiredPhoto>();
        CHECK_EQ(content_type_name(content.get()), "other");
    }

    // is_user_message: user-authored content passes, service/system content
    // (and anything unknown) is filtered.
    {
        auto text = td_api::make_object<td_api::messageText>();
        text->text_ = ftext("hi");
        CHECK(is_user_message(text.get()));
        CHECK(is_user_message(td_api::make_object<td_api::messagePhoto>().get()));
        CHECK(is_user_message(td_api::make_object<td_api::messageDice>().get()));
        CHECK(is_user_message(td_api::make_object<td_api::messageCall>().get()));
        CHECK(is_user_message(td_api::make_object<td_api::messagePoll>().get()));

        CHECK(!is_user_message(nullptr));
        CHECK(!is_user_message(td_api::make_object<td_api::messagePinMessage>().get()));
        CHECK(!is_user_message(td_api::make_object<td_api::messageChatAddMembers>().get()));
        CHECK(!is_user_message(td_api::make_object<td_api::messageContactRegistered>().get()));
        CHECK(!is_user_message(td_api::make_object<td_api::messageChatChangeTitle>().get()));
        CHECK(!is_user_message(td_api::make_object<td_api::messageVideoChatStarted>().get()));
        CHECK(!is_user_message(td_api::make_object<td_api::messageExpiredPhoto>().get()));
        CHECK(!is_user_message(td_api::make_object<td_api::messageUnsupported>().get()));
    }

    // Reply within the same chat is reported; cross-chat replies are not.
    {
        auto msg = base_message();
        auto reply = td_api::make_object<td_api::messageReplyToMessage>();
        reply->chat_id_ = 42;
        reply->message_id_ = 99;
        msg->reply_to_ = std::move(reply);
        CHECK(reply_to_message_id(*msg) == 99);

        auto other = base_message();
        auto cross = td_api::make_object<td_api::messageReplyToMessage>();
        cross->chat_id_ = 43; // different chat
        cross->message_id_ = 99;
        other->reply_to_ = std::move(cross);
        CHECK(reply_to_message_id(*other) == 0);

        CHECK(reply_to_message_id(*base_message()) == 0); // no reply at all
    }

    // Sender that is a chat (channel posts).
    {
        auto sender = td_api::make_object<td_api::messageSenderChat>();
        sender->chat_id_ = -100555;
        td_api::object_ptr<td_api::MessageSender> s = std::move(sender);
        CHECK(sender_id_of(s.get()) == -100555);
        CHECK(sender_id_of(nullptr) == 0);
    }

    RETURN_TEST_RESULT();
}
