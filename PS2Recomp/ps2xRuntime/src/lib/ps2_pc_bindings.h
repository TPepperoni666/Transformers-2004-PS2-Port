#pragma once
#include "ps2_pad_map.h"
namespace ps2x::input {
    inline int keyFromName(std::string n)
    {
        n = ps2_padmap::lowerCopy(n);
        if (n.rfind("key_", 0) == 0)
            n = n.substr(4);
        if (n.size() == 1 && n[0] >= 'a' && n[0] <= 'z')
            return KEY_A + (n[0] - 'a');
        if (n.size() == 1 && n[0] >= '0' && n[0] <= '9')
            return KEY_ZERO + (n[0] - '0');
        static const std::pair<const char *, int> named[] = {
            {"zero", KEY_ZERO}, {"one", KEY_ONE}, {"two", KEY_TWO}, {"three", KEY_THREE}, {"four", KEY_FOUR},
            {"five", KEY_FIVE}, {"six", KEY_SIX}, {"seven", KEY_SEVEN}, {"eight", KEY_EIGHT}, {"nine", KEY_NINE},
            {"space", KEY_SPACE}, {"enter", KEY_ENTER}, {"return", KEY_ENTER}, {"tab", KEY_TAB},
            {"backspace", KEY_BACKSPACE}, {"up", KEY_UP}, {"down", KEY_DOWN}, {"left", KEY_LEFT}, {"right", KEY_RIGHT},
            {"left_shift", KEY_LEFT_SHIFT}, {"lshift", KEY_LEFT_SHIFT}, {"right_shift", KEY_RIGHT_SHIFT},
            {"rshift", KEY_RIGHT_SHIFT}, {"left_control", KEY_LEFT_CONTROL}, {"lctrl", KEY_LEFT_CONTROL},
            {"left_ctrl", KEY_LEFT_CONTROL}, {"right_control", KEY_RIGHT_CONTROL}, {"rctrl", KEY_RIGHT_CONTROL},
            {"right_ctrl", KEY_RIGHT_CONTROL}, {"left_alt", KEY_LEFT_ALT}, {"lalt", KEY_LEFT_ALT},
            {"right_alt", KEY_RIGHT_ALT}, {"ralt", KEY_RIGHT_ALT}, {"insert", KEY_INSERT}, {"delete", KEY_DELETE},
            {"home", KEY_HOME}, {"end", KEY_END}, {"page_up", KEY_PAGE_UP}, {"pageup", KEY_PAGE_UP},
            {"page_down", KEY_PAGE_DOWN}, {"pagedown", KEY_PAGE_DOWN}, {"comma", KEY_COMMA}, {"period", KEY_PERIOD},
            {"slash", KEY_SLASH}, {"semicolon", KEY_SEMICOLON}, {"apostrophe", KEY_APOSTROPHE}, {"minus", KEY_MINUS},
            {"equal", KEY_EQUAL}, {"left_bracket", KEY_LEFT_BRACKET}, {"right_bracket", KEY_RIGHT_BRACKET},
            {"backslash", KEY_BACKSLASH}, {"grave", KEY_GRAVE}, {"kp_0", KEY_KP_0}, {"kp_1", KEY_KP_1},
            {"kp_2", KEY_KP_2}, {"kp_3", KEY_KP_3}, {"kp_4", KEY_KP_4}, {"kp_5", KEY_KP_5}, {"kp_6", KEY_KP_6},
            {"kp_7", KEY_KP_7}, {"kp_8", KEY_KP_8}, {"kp_9", KEY_KP_9}, {"kp_enter", KEY_KP_ENTER},
            {"kp_add", KEY_KP_ADD}, {"kp_subtract", KEY_KP_SUBTRACT}, {"kp_multiply", KEY_KP_MULTIPLY},
            {"kp_divide", KEY_KP_DIVIDE}, {"kp_decimal", KEY_KP_DECIMAL}};
        for (const auto &[name, key] : named)
            if (n == name)
                return key;
        return -1; // unknown (F-keys and Esc are reserved by the runtime)
    }

inline std::string keyNameFromCode(int code) {
 if(code>=KEY_A && code<=KEY_Z) return std::string(1,char('A'+code-KEY_A));
 if(code>=KEY_ZERO && code<=KEY_NINE) return std::string(1,char('0'+code-KEY_ZERO));
 static const char* names[]={"zero","one","two","three","four","five","six","seven","eight","nine","space","enter","return","tab","backspace","up","down","left","right","left_shift","lshift","right_shift","rshift","left_control","lctrl","left_ctrl","right_control","rctrl","right_ctrl","left_alt","lalt","right_alt","ralt","insert","delete","home","end","page_up","pageup","page_down","pagedown","comma","period","slash","semicolon","apostrophe","minus","equal","left_bracket","right_bracket","backslash","grave","kp_0","kp_1","kp_2","kp_3","kp_4","kp_5","kp_6","kp_7","kp_8","kp_9","kp_enter","kp_add","kp_subtract","kp_multiply","kp_divide","kp_decimal",nullptr};
 for(int i=0;names[i];++i) if(keyFromName(names[i])==code) return names[i];
 return {};
}
inline std::string padNameFromSource(int source) {
 if(source==ps2_padmap::SRC_LT_AXIS) return "LEFT_TRIGGER_2";
 if(source==ps2_padmap::SRC_RT_AXIS) return "RIGHT_TRIGGER_2";
 static const char* names[]={"LEFT_FACE_UP","LEFT_FACE_DOWN","LEFT_FACE_LEFT","LEFT_FACE_RIGHT","RIGHT_FACE_UP","RIGHT_FACE_DOWN","RIGHT_FACE_LEFT","RIGHT_FACE_RIGHT","LEFT_TRIGGER_1","RIGHT_TRIGGER_1","LEFT_THUMB","RIGHT_THUMB","MIDDLE","MIDDLE_LEFT","MIDDLE_RIGHT","lt_button","rt_button",nullptr};
 for(int i=0;names[i];++i) if(ps2_padmap::padSourceFromName(names[i])==source) return names[i];
 return {};
}
}
