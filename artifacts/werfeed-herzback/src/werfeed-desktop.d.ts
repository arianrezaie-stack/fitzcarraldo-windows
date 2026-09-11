export {};

declare global {
  interface Window {
    werfeedDesktop?: {
      platform: string;
      desktopShell: boolean;
      engine: {
        getStatus: () => Promise<{ state: string; reason?: string }>;
        command: (command: 'list_devices' | 'configure' | 'start' | 'stop' | 'set_route_arming' | 'restart_audio' | 'set_protection' | 'start_calibration' | 'reset_calibration', payload?: Record<string, unknown>) => Promise<{ accepted: true }>;
        onEvent: (listener: (event: unknown) => void) => () => void;
        onStatus: (listener: (status: { state: string; reason?: string }) => void) => () => void;
        validation?: {
          reportDevices: (devices: unknown[], pairs: unknown[]) => void;
        };
      };
    };
  }
}