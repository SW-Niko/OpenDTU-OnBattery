export interface AbsorptionMode {
    enabled: boolean;
    state: string;
    regulation_quality: string;
    duration: string;
}

export interface BulkMode {
    enabled: boolean;
    state: string;
    slope_enabled: boolean;
    slope_power: number;
    max_slope_power: number;
    battery_reserve_power: number;
    duration: string;
}

export interface SurplusStatus {
    enabled: boolean;
    requirements_check: string;
    state: string;
    surplus_power: number;
    max_surplus_power: number;
    bulk_mode: BulkMode;
    absorption_mode: AbsorptionMode;
}
