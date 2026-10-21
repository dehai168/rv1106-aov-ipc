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

export type VideoOsdConfig = {
  enabled: boolean
  show_time: boolean
  show_name: boolean
  pos_x: number
  pos_y: number
  name: string
}

export function getVideoOsd() {
  return request<VideoOsdConfig>('/api/v1/video/osd')
}

export function setVideoOsd(body: Partial<VideoOsdConfig> & { apply?: boolean }) {
  return request<VideoOsdConfig>('/api/v1/video/osd', {
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

export function getStorageRecords(params?: {
  date?: string
  limit?: number
  offset?: number
}) {
  const q: string[] = []
  if (params?.date) q.push(`date=${encodeURIComponent(params.date)}`)
  if (params?.limit) q.push(`limit=${encodeURIComponent(String(params.limit))}`)
  if (params?.offset != null) q.push(`offset=${encodeURIComponent(String(params.offset))}`)
  const qs = q.length ? `?${q.join('&')}` : ''
  return request<{
    records: StorageRecord[]
    total: number
    count: number
    offset?: number
    limit?: number
  }>(`/api/v1/storage/records${qs}`)
}

export function formatStorage() {
  return request<{ deleted: number; note: string }>('/api/v1/storage/format', {
    method: 'POST',
    body: JSON.stringify({ confirm: true }),
  })
}

export type RecordSchedule = {
  enabled: boolean
  grid: string
}

export type RecordConfig = {
  mode: 'motion' | 'continuous' | 'schedule' | string
  segment_sec: number
  pre_record_sec: number
  quiet_sec: number
  recycle_free_percent: number
  schedule: RecordSchedule
  runtime_mode?: string
  running?: boolean
  policy_hold?: boolean
}

export function getRecordConfig() {
  return request<RecordConfig>('/api/v1/record')
}

export function setRecordConfig(
  body: Partial<
    Pick<
      RecordConfig,
      'mode' | 'segment_sec' | 'pre_record_sec' | 'quiet_sec' | 'recycle_free_percent' | 'schedule'
    >
  > & { apply?: boolean },
) {
  return request<RecordConfig>('/api/v1/record', {
    method: 'POST',
    body: JSON.stringify(body),
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
  /** 168 chars '0'|'1', Mon..Sun x hour0..23 */
  grid?: string
}

export type AlarmLink = {
  record: boolean
  snapshot: boolean
}

export type AlarmMotion = {
  enabled: boolean
  sensitivity: number
  square_pct: number
  hit_frames: number
  region: AlarmRegion
  regions: AlarmRegion[]
  schedule: AlarmSchedule
  link: AlarmLink
  running: boolean
  motion_count: number
  last_event: AlarmEvent
}

export function getAlarmMotion() {
  return request<AlarmMotion>('/api/v1/alarm/motion')
}

export function setAlarmMotion(
  body: Partial<
    Pick<
      AlarmMotion,
      'enabled' | 'sensitivity' | 'square_pct' | 'hit_frames' | 'region' | 'regions' | 'schedule' | 'link'
    >
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

export type FaceRegion = AlarmRegion

export type FaceConfig = {
  enabled: boolean
  model: string
  trigger: string
  model_path: string
  sensitivity: 'high' | 'normal' | 'low' | string
  interval_ms: number
  hold_ms: number
  cooldown_ms: number
  confirm_frames: number
  min_face_px: number
  max_faces: number
  max_snap_per_frame: number
  face_jpeg_side: number
  score_thr_x100: number
  margin_x100: number
  save_scene: boolean
  regions: FaceRegion[]
  schedule: { enabled: boolean; grid: string }
  link: { record: boolean; snapshot: boolean; alarm: boolean }
  quota_percent: number
  daily_limit: number
  running: boolean
  snap_count: number
  infer_count: number
  last_event: {
    ts?: number
    score?: number
    x?: number
    y?: number
    w?: number
    h?: number
    face_file?: string
    scene_file?: string
  }
}

export type FaceEventItem = {
  type: string
  face_file: string
  file: string
}

export function getFaceConfig() {
  return request<FaceConfig>('/api/v1/alarm/face')
}

export function setFaceConfig(
  body: Partial<
    Pick<
      FaceConfig,
      | 'enabled'
      | 'sensitivity'
      | 'interval_ms'
      | 'hold_ms'
      | 'cooldown_ms'
      | 'confirm_frames'
      | 'min_face_px'
      | 'max_snap_per_frame'
      | 'save_scene'
      | 'regions'
      | 'schedule'
      | 'link'
      | 'quota_percent'
      | 'daily_limit'
    >
  > & { apply?: boolean },
) {
  return request<FaceConfig>('/api/v1/alarm/face', {
    method: 'POST',
    body: JSON.stringify(body),
  })
}

export function getFaceEvents(opts: { date?: string; limit?: number; offset?: number } = {}) {
  const q = new URLSearchParams()
  if (opts.date) q.set('date', opts.date)
  if (opts.limit != null) q.set('limit', String(opts.limit))
  if (opts.offset != null) q.set('offset', String(opts.offset))
  const qs = q.toString()
  return request<{ date: string; total: number; count: number; offset: number; limit: number; events: FaceEventItem[] }>(
    `/api/v1/alarm/face/events${qs ? `?${qs}` : ''}`,
  )
}

export function faceImageUrl(file: string) {
  return `/api/v1/alarm/face/image?file=${encodeURIComponent(file)}`
}

export function deleteFaceEvents(body: { files?: string[]; date?: string }) {
  return request<{ deleted: number }>('/api/v1/alarm/face/events', {
    method: 'DELETE',
    body: JSON.stringify(body),
  })
}

export function previewSnapshot() {
  return request<{ file: string; url: string }>('/api/v1/preview/snapshot', {
    method: 'POST',
    body: '{}',
  })
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

/** Public branding name (no auth). */
export function getDeviceName() {
  return request<{ device_name: string }>('/api/v1/system/device_name')
}

export function setSystemInfo(body: { device_name: string }) {
  return request<SystemInfo>('/api/v1/system/info', {
    method: 'POST',
    body: JSON.stringify(body),
  })
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

export function getSystemLogTail(tail = 200, file = 'ipc_app.log') {
  return request<{ file: string; tail: number; lines: string[] }>(
    `/api/v1/system/log?file=${encodeURIComponent(file)}&tail=${encodeURIComponent(String(tail))}`,
  )
}

export type RebootSchedule = {
  enabled: boolean
  hour: number
  minute: number
  days: string
}

export function getRebootSchedule() {
  return request<RebootSchedule>('/api/v1/system/reboot_schedule')
}

export function setRebootSchedule(body: Partial<RebootSchedule>) {
  return request<RebootSchedule>('/api/v1/system/reboot_schedule', {
    method: 'POST',
    body: JSON.stringify(body),
  })
}
