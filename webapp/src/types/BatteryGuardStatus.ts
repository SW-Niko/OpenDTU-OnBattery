export interface Values {
    voltage_resolution: number;
    current_resolution: number;
    open_circuit_voltage_calculated: boolean;
    open_circuit_voltage: number;
    uncompensated_voltage: number;
    internal_resistance_calculated: boolean;
    resistance_configured: number;
    resistance_calculated: number;
    resistance_calculation_count: number;
    resistance_calculation_state: string;
    measurement_time_period: number;
    v_i_time_stamp_delay: number;
}

export interface Compensation {
    enabled: boolean;
    state: string;
    used_resistance: number;
    open_circuit_voltage: number;
}

export interface Limits {
    max_voltage_resolution: number;
    max_current_resolution: number;
    max_measurement_time_period: number;
    max_v_i_time_stamp_delay: number;
    min_resistance_calculation_count: number;
}

export interface Recharge {
    enabled: boolean;
    state: string;
    day: number;
    use_voltage_thresholds: boolean;
    start_threshold: number;
    stop_threshold: number;
    soc_start_threshold: number;
    soc_stop_threshold: number;
    power_limit: number;
}

export interface Limiter {
    enabled: boolean;
    state: string;
    active: boolean;
    power_limit: number;
    quality: string;
    active_time: string;
    stop_time: string;
    stop_reason: string;
}

export interface BatteryGuardStatus {
    enabled: boolean;
    values: Values;
    limits: Limits;
    recharge: Recharge;
    compensation: Compensation;
    limiter: Limiter;
}
