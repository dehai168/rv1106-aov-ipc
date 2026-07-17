export type ApiResp<T> = { code: number; msg: string; data: T }

async function request<T>(path: string, init?: RequestInit): Promise<ApiResp<T>> {
  const r = await fetch(path, {
    credentials: 'include',
    headers: { 'Content-Type': 'application/json', ...(init?.headers || {}) },
    ...init,
  })
  return r.json() as Promise<ApiResp<T>>
}

export function login(username: string, password: string) {
  return request<{ token: string; must_change: boolean; username: string }>(
    '/api/v1/auth/login',
    { method: 'POST', body: JSON.stringify({ username, password }) },
  )
}

export function logout() {
  return request<Record<string, never>>('/api/v1/auth/logout', { method: 'POST' })
}

export function me() {
  return request<{ username: string; must_change: boolean }>('/api/v1/auth/me')
}

export function changePassword(old_password: string, new_password: string) {
  return request<Record<string, never>>('/api/v1/auth/password', {
    method: 'POST',
    body: JSON.stringify({ old_password, new_password }),
  })
}

export type NetworkConfig = {
  iface: string
  mode: 'dhcp' | 'static' | string
  ip: string
  netmask: string
  gateway: string
  dns1: string
  dns2: string
  link: string
  current_ip: string
  current_netmask: string
  current_gateway: string
  usb0_ip: string
}

export function getNetwork() {
  return request<NetworkConfig>('/api/v1/network/config')
}

export function setNetwork(
  body: Partial<NetworkConfig> & { apply?: boolean },
) {
  return request<NetworkConfig>('/api/v1/network/config', {
    method: 'POST',
    body: JSON.stringify(body),
  })
}

export type VideoImageConfig = {
  brightness: number
  contrast: number
  saturation: number
  mirror: boolean
  flip: boolean
}

export type StreamParams = {
  w: number
  h: number
  fps: number
  bitrate_kbps: number
  gop: number
}

export type VideoEncodeConfig = {
  main: StreamParams
  sub: StreamParams
  stream_up: boolean
}

export function getVideoImage() {
  return request<VideoImageConfig>('/api/v1/video/image')
}

export function setVideoImage(body: Partial<VideoImageConfig> & { apply?: boolean }) {
  return request<VideoImageConfig>('/api/v1/video/image', {
    method: 'POST',
    body: JSON.stringify(body),
  })
}

export function getVideoEncode() {
  return request<VideoEncodeConfig>('/api/v1/video/encode')
}

export function setVideoEncode(body: Partial<VideoEncodeConfig> & { apply?: boolean }) {
  return request<VideoEncodeConfig>('/api/v1/video/encode', {
    method: 'POST',
    body: JSON.stringify(body),
  })
}

export type StorageStatus = {
  mounted: boolean
  mount_path: string
  fstype: string
  total_bytes: number
  free_bytes: number
  free_percent: number
}

export type StorageRecord = {
  path: string
  name: string
  mtime: number
  size: number
}

export function getStorageStatus() {
  return request<StorageStatus>('/api/v1/storage/status')
}

export function getStorageRecords(params?: { date?: string; limit?: number }) {
  const q: string[] = []
  if (params?.date) q.push(`date=${encodeURIComponent(params.date)}`)
  if (params?.limit) q.push(`limit=${encodeURIComponent(String(params.limit))}`)
  const qs = q.length ? `?${q.join('&')}` : ''
  return request<{ records: StorageRecord[]; total: number; count: number }>(
    `/api/v1/storage/records${qs}`,
  )
}

export function formatStorage() {
  return request<{ deleted: number; note: string }>('/api/v1/storage/format', {
    method: 'POST',
    body: JSON.stringify({ confirm: true }),
  })
}

export type AlarmEvent = {
  ts: number
  square: number
  pct_x10: number
  rect: [number, number, number, number]
  snapshot?: string
}

export type AlarmRegion = {
  enabled: boolean
  x: number
  y: number
  w: number
  h: number
}

export type AlarmSchedule = {
  enabled: boolean
  start_min: number
  end_min: number
  days: number
}

export type AlarmMotion = {
  enabled: boolean
  sensitivity: number
  square_pct: number
  hit_frames: number
  region: AlarmRegion
  schedule: AlarmSchedule
  running: boolean
  motion_count: number
  last_event: AlarmEvent
}

export function getAlarmMotion() {
  return request<AlarmMotion>('/api/v1/alarm/motion')
}

export function setAlarmMotion(
  body: Partial<
    Pick<AlarmMotion, 'enabled' | 'sensitivity' | 'square_pct' | 'hit_frames' | 'region' | 'schedule'>
  > & {
    apply?: boolean
  },
) {
  return request<AlarmMotion>('/api/v1/alarm/motion', {
    method: 'POST',
    body: JSON.stringify(body),
  })
}

export function getAlarmEvents(limit: number = 50) {
  return request<{ count: number; events: AlarmEvent[] }>(
    `/api/v1/alarm/events?limit=${encodeURIComponent(String(limit))}`,
  )
}

export function alarmSnapshotUrl(file: string) {
  return `/api/v1/alarm/snapshot?file=${encodeURIComponent(file)}`
}

export type SystemInfo = {
  device_name: string
  hostname: string
  model: string
  version: string
  uptime_sec: number
  mem_total_kb: number
  mem_free_kb: number
}

export type SystemTimeConfig = {
  unix_time: number
  timezone: string
  ntp_enabled: boolean
  ntp_server: string
}

export function getSystemInfo() {
  return request<SystemInfo>('/api/v1/system/info')
}

export function getSystemTime() {
  return request<SystemTimeConfig>('/api/v1/system/time')
}

export function setSystemTime(
  body: Partial<SystemTimeConfig> & { apply_ntp?: boolean },
) {
  return request<SystemTimeConfig>('/api/v1/system/time', {
    method: 'POST',
    body: JSON.stringify(body),
  })
}

export function rebootSystem() {
  return request<Record<string, never>>('/api/v1/system/reboot', {
    method: 'POST',
    body: JSON.stringify({ confirm: true }),
  })
}

export function resetSystem() {
  return request<Record<string, never>>('/api/v1/system/reset', {
    method: 'POST',
    body: JSON.stringify({ confirm: true }),
  })
}

export function systemLogUrl(file = 'ipc_app.log') {
  return `/api/v1/system/log?file=${encodeURIComponent(file)}`
}
