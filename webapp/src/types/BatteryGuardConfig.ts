export interface BatteryGuardConfig {
    enabled: boolean;
    internal_resistance: number;
    voltage_drop_compensation_enabled: boolean;
    low_voltage_limiter_enabled: boolean;
    dpl_start_threshold: number;
    dpl_stop_threshold: number;
    recharge_helper_enabled: boolean;
    excessive_solar_power_disabled: boolean;
    duration_idle: number;
    duration_stage1: number;
    duration_stage2: number;
    max_start_threshold: number;
    max_stop_threshold: number;
    use_voltage_thresholds: boolean;
    max_soc_start_threshold: number;
    max_soc_stop_threshold: number;
    upper_power_limit: number;
}

// meta-data not directly part of the BatteryGuard settings,
// to control visibility of BatteryGuard settings
export interface BatteryGuardMetaData {
    battery_enabled: boolean;
}
