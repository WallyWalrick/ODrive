#ifndef __ENDSTOP_HPP
#define __ENDSTOP_HPP

class Endstop {
   public:
    struct Config_t {
        uint16_t gpio_num;
        bool enabled = false;
        int32_t offset = 0;
        bool is_active_high = false;
        float debounce_ms = 100.0f;
        float home_percentage = 0.0f;
        bool physical_endstop = false;
        int32_t min_ms_homing = 4000;
    };

    Endstop(Endstop::Config_t& config);

    Endstop::Config_t& config_;
    Axis* axis_ = nullptr;

    void set_endstop_enabled(bool enable);
    void endstop_cb();
    void update();

    bool getEndstopState();

    bool endstop_state_ = false;
    int32_t offset_from_home = 0;

    auto make_protocol_definitions() {
        return make_protocol_member_list(
            make_protocol_ro_property("endstop_state_", &endstop_state_),
            make_protocol_ro_property("offset_from_home", &offset_from_home),            
            make_protocol_object("config",
                                 make_protocol_property("gpio_num", &config_.gpio_num),
                                 make_protocol_property("enabled", &config_.enabled),
                                 make_protocol_property("offset", &config_.offset),
                                 make_protocol_property("is_active_high", &config_.is_active_high),
                                 make_protocol_property("debounce_ms", &config_.debounce_ms),
                                 make_protocol_property("home_percentage", &config_.home_percentage),
                                 make_protocol_property("physical_endstop", &config_.physical_endstop),
                                 make_protocol_property("min_ms_homing", &config_.min_ms_homing)));
    }

   private:
    bool pin_state_ = false;
    volatile float debounce_timer_ = 0;
};
#endif