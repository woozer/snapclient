#pragma once
#include <stdbool.h>
#include <math.h>
#include <stdlib.h>

// These are bounded bench profiles, not total-source power ratings.
// In dock mode BQ IBUS excludes the amplifier/3V3 bypass current.
typedef enum { CR2_USB5=0, CR2_DOCK19=1, CR2_BATTERY4S=2, CR2_USB_PD=3 } cr2_power_profile_t;

static inline const char *cr2_profile_name(cr2_power_profile_t profile) {
    switch(profile) {
    case CR2_USB5:return "usb5";
    case CR2_DOCK19:return "dock19";
    case CR2_BATTERY4S:return "battery4s";
    case CR2_USB_PD:return "usb-pd";
    default:return "invalid";
    }
}

static inline bool cr2_selector_allowed(cr2_power_profile_t profile, int selector) {
    return profile==CR2_USB_PD || (profile==CR2_DOCK19 && selector==1) ||
        ((profile==CR2_USB5 || profile==CR2_BATTERY4S) && selector==0);
}

static inline bool cr2_supply_allowed(cr2_power_profile_t profile, bool valid, int vbus,
                                     int sys, int ibus, int selector, int charge,
                                     int vbat, int fault0, int fault1, int die_half_c) {
    if(fault0 || fault1 || !cr2_selector_allowed(profile,selector))return false;
    if(profile==CR2_BATTERY4S) {
        // Conservative attended 4S bench window, not a pack/BMS rating.
        // IBUS is input current; no battery-current/power claim is made here.
        return valid && !charge && vbus>=0 && vbus<=1000 &&
            vbat>=13600 && vbat<=16800 && sys>=13000 && sys<=17200 &&
            abs(sys-vbat)<=800 && ibus>=-50 && ibus<=50 && die_half_c<120;
    }
    if(!valid || charge || ibus<0 || sys<5000)return false;
    // Also requires a fresh matching HUSB explicit contract + USB selected.
    if(profile==CR2_USB_PD)return die_half_c<120 && ibus<=4000 && sys<=20500 &&
        ((vbus>=4500 && vbus<=5500 && selector==0) ||
         (vbus>=8000 && vbus<=20500 && selector==1));
    if(ibus>450)return false;
    if(profile==CR2_DOCK19)return vbus>=18000 && vbus<=20500 && sys<=20500 && selector==1;
    return vbus>=4500 && vbus<=5500 && sys<=22000 && selector==0;
}

// Dock step is +3.5dB (~1.496x amplitude) from the tested -18dB profile.
static inline double cr2_ceiling_db(cr2_power_profile_t profile) {
    // User-requested battery step: +6dB from the tested -30dB ceiling.
    return profile==CR2_DOCK19 ? -14.5 : -24.0;
}

static inline unsigned cr2_limited_gain(cr2_power_profile_t profile, int volume) {
    if(volume<=0)return 0xff;
    if(volume>100)volume=100;
    double db=cr2_ceiling_db(profile)+20.0*log10(volume/100.0);
    int reg=(int)ceil((24.0-db)*2.0);
    return reg>254 ? 254 : (unsigned)reg;
}

// Web sessions use a perceptual 60dB slider. Legacy timed bench requests keep
// their original gain mapping above. No digital boost beyond 0dB is allowed.
// TAS5825M table9-2: -7dB analog gives 13.18V peak at digital full scale;
// the mandatory host crossover retains another12dB of source headroom.
static inline unsigned cr2_ui_analog(cr2_power_profile_t profile) {
    return profile==CR2_USB5 ? 31 : 14;
}
static inline double cr2_ui_ceiling_db(cr2_power_profile_t profile) {
    return profile==CR2_USB5 ? -24.0 : 0.0;
}
static inline unsigned cr2_ui_gain(cr2_power_profile_t profile, int volume) {
    if(volume<=0)return 255;
    if(volume>100)volume=100;
    double db=cr2_ui_ceiling_db(profile)-0.6*(100-volume);
    return (unsigned)ceil((24.0-db)*2.0);
}

static inline unsigned cr2_pd_mv(unsigned status0) {
    static const unsigned mv[]={0,5000,9000,12000,15000,18000,20000};
    unsigned code=status0>>4;return code<7?mv[code]:0;
}
static inline unsigned cr2_pd_ma(unsigned status0) {
    static const unsigned ma[]={500,700,1000,1250,1500,1750,2000,2250,2500,2750,3000,3250,3500,4000,4500,5000};
    return ma[status0&15];
}
static inline bool cr2_pd_matches(unsigned s0,unsigned s1,unsigned offer,int measured_mv,int input_st) {
    unsigned mv=cr2_pd_mv(s0);
    return input_st==0 && (s1&0x40) && mv && (offer&0x80) &&
        cr2_pd_ma(s0)<=cr2_pd_ma(offer) && abs(measured_mv-(int)mv)<=(int)mv/20 &&
        measured_mv>=4500 && measured_mv<=20500;
}
// Conservative source-budget model, not measured draw/current regulation:
// retain20% source margin, reserve1.5W for logic, assume80% conversion,
// and allow simultaneous full-scale square waves into BOTH4ohm channels.
static inline unsigned cr2_pd_analog(unsigned mv) {
    int reg=(int)ceil(40.0*log10(29.5/(0.85*mv/1000.0)));
    return reg<14?14:reg>31?31:(unsigned)reg;
}
static inline unsigned cr2_pd_ceiling_reg(unsigned mv,unsigned ma) {
    double audio=fmax(0.0,mv*ma/1000000.0*0.8-1.5)*0.8;
    if(audio<=0)return 254;
    double peak=fmin(0.85*mv/1000.0,sqrt(audio*2.0));
    double analog=29.5*pow(10.0,-0.5*cr2_pd_analog(mv)/20.0);
    double db=fmin(0.0,20.0*log10(peak/analog));
    return (unsigned)ceil((24.0-db)*2.0);
}
