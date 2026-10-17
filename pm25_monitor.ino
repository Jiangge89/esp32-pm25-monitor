// =====================================================
//  ESP32 PM2.5 检测仪（带 24 小时趋势曲线）
//  传感器：PMS5003
//  不需要安装额外的库
//  浏览器打开 http://air.local 或串口打印的 IP 地址
// =====================================================

#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <time.h>

// ---------- 改成你的 WiFi（必须是 2.4GHz）----------
const char* WIFI_SSID = "你的WiFi名";
const char* WIFI_PASS = "你的WiFi密码";
const char* HOST_NAME = "air";          // http://air.local

// ---------- 引脚 ----------
const int PMS_RX_PIN = 16;   // D16 / RX2，接 PMS5003 的 TX
const int PMS_TX_PIN = 17;   // D17 / TX2，接 PMS5003 的 RX

// ---------- 记录设置 ----------
const int SAMPLE_SEC  = 300;   // 每 5 分钟记录一个点
const int HISTORY_LEN = 288;   // 288 × 5 分钟 = 24 小时
const int WARMUP_MS   = 30000; // 开机前 30 秒不记录（传感器预热）

WebServer server(80);

// ---------- 当前读数 ----------
uint16_t pm1 = 0, pm25 = 0, pm10 = 0;
unsigned long lastPmsOk = 0;
unsigned long pmsBytes = 0;

// ---------- 历史记录 ----------
struct Sample {
  uint32_t t;      // 时间戳（秒），未同步时间时为 0
  uint16_t pm25;
};
Sample history[HISTORY_LEN];
int histCount = 0;
int histHead  = 0;

uint32_t pmSum = 0;
uint16_t pmN = 0;
unsigned long lastSample = 0;

// =====================================================
//  读取 PMS5003
// =====================================================
bool readPMS() {
  bool got = false;
  while (Serial2.available() >= 32) {
    if (Serial2.peek() != 0x42) { Serial2.read(); pmsBytes++; continue; }
    uint8_t buf[32];
    Serial2.readBytes(buf, 32);
    pmsBytes += 32;
    if (buf[1] != 0x4D) continue;

    uint16_t sum = 0;
    for (int i = 0; i < 30; i++) sum += buf[i];
    if (sum != ((buf[30] << 8) | buf[31])) continue;

    pm1  = (buf[10] << 8) | buf[11];
    pm25 = (buf[12] << 8) | buf[13];
    pm10 = (buf[14] << 8) | buf[15];
    got = true;
  }
  return got;
}

bool pmsOnline() { return millis() - lastPmsOk < 5000; }

const char* pmLevel(uint16_t v) {
  if (v <= 35)  return "优";
  if (v <= 75)  return "良";
  if (v <= 115) return "轻度污染";
  if (v <= 150) return "中度污染";
  if (v <= 250) return "重度污染";
  return "严重污染";
}

// =====================================================
//  记录一个历史点（5 分钟平均）
// =====================================================
void addSample() {
  if (pmN == 0) return;

  time_t now;
  time(&now);
  Sample s;
  s.t    = (now > 1700000000) ? (uint32_t)now : 0;
  s.pm25 = pmSum / pmN;

  history[histHead] = s;
  histHead = (histHead + 1) % HISTORY_LEN;
  if (histCount < HISTORY_LEN) histCount++;

  Serial.printf("📈 记录第 %d 个点：PM2.5 平均 %u\n", histCount, s.pm25);
  pmSum = 0;
  pmN = 0;
}

// =====================================================
//  网页
// =====================================================
const char PAGE[] PROGMEM = R"rawliteral(
<!DOCTYPE html><html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>PM2.5 检测仪</title>
<style>
body{font-family:sans-serif;background:#f3f4f6;margin:0;padding:16px;color:#222}
h1{font-size:20px;margin:0 0 12px}
h2{font-size:15px;margin:0 0 8px;color:#555}
.box{background:#fff;border-radius:14px;padding:16px;margin-bottom:12px}
.big{text-align:center}
.num{font-size:60px;font-weight:bold;line-height:1.1}
.lvl{font-weight:bold;margin-top:4px}
.grid{display:grid;grid-template-columns:1fr 1fr;gap:12px;margin-bottom:12px}
.card{background:#fff;border-radius:14px;padding:12px;text-align:center}
.v{font-size:26px;font-weight:bold}.l{color:#777;font-size:13px}
canvas{width:100%;height:220px;display:block}
.stats{display:flex;justify-content:space-around;margin-top:10px;font-size:13px;color:#555}
.stats b{display:block;font-size:18px;color:#222}
.tip{color:#999;font-size:12px;text-align:center}
</style></head><body>
<h1>PM2.5 检测仪</h1>

<div class="box big">
  <div class="l">PM2.5（μg/m³）</div>
  <div class="num" id="pm25">--</div>
  <div class="lvl" id="lvl"></div>
</div>

<div class="grid">
  <div class="card"><div class="v" id="pm1">--</div><div class="l">PM1.0</div></div>
  <div class="card"><div class="v" id="pm10">--</div><div class="l">PM10</div></div>
</div>

<div class="box">
  <h2>PM2.5 · 过去 24 小时</h2>
  <canvas id="c"></canvas>
  <div class="stats">
    <div><b id="smax">--</b>最高</div>
    <div><b id="savg">--</b>平均</div>
    <div><b id="smin">--</b>最低</div>
  </div>
</div>
<div class="tip">每 5 分钟记录一个点 · 断电后记录会清空</div>

<script>
function level(v){
  if(v<=35) return ['优','#2e9e44'];
  if(v<=75) return ['良','#c9a800'];
  if(v<=115) return ['轻度污染','#e07b00'];
  if(v<=150) return ['中度污染','#d03030'];
  if(v<=250) return ['重度污染','#8e2c8e'];
  return ['严重污染','#7a1f1f'];
}
const $=id=>document.getElementById(id);

async function loadNow(){
  try{
    const j=await (await fetch('/json')).json();
    if(j.online){
      const [t,c]=level(j.pm25);
      $('pm25').textContent=j.pm25; $('pm25').style.color=c;
      $('lvl').textContent=t; $('lvl').style.color=c;
      $('pm1').textContent=j.pm1; $('pm10').textContent=j.pm10;
    }else{
      $('pm25').textContent='--'; $('pm25').style.color='#aaa';
      $('lvl').textContent='传感器无数据'; $('lvl').style.color='#aaa';
    }
  }catch(e){}
}

let lastHist=null;
async function loadHistory(){
  try{ lastHist=await (await fetch('/history')).json(); draw(lastHist); }catch(e){}
}

function draw(j){
  const c=$('c'), dpr=window.devicePixelRatio||1;
  const W=c.clientWidth, H=c.clientHeight;
  c.width=W*dpr; c.height=H*dpr;
  const g=c.getContext('2d'); g.scale(dpr,dpr); g.clearRect(0,0,W,H);
  const P={l:34,r:8,t:8,b:22};
  const d=j.data;

  if(d.length<2){
    g.fillStyle='#999'; g.font='13px sans-serif';
    g.fillText('数据收集中，每 5 分钟记录一个点…',P.l,H/2);
    return;
  }

  const vals=d.map(p=>p[1]);
  $('smax').textContent=Math.max(...vals);
  $('smin').textContent=Math.min(...vals);
  $('savg').textContent=Math.round(vals.reduce((a,b)=>a+b,0)/vals.length);

  const now=Date.now()/1000;
  const xs=d.map((p,i)=>p[0]>0?p[0]:now-(d.length-1-i)*j.interval);
  const x1=xs[xs.length-1], x0=x1-86400;
  let ymax=Math.max(50,...vals); ymax=Math.ceil(ymax/50)*50;
  const X=v=>P.l+(v-x0)/(x1-x0)*(W-P.l-P.r);
  const Y=v=>H-P.b-(v/ymax)*(H-P.t-P.b);

  [[0,35,'#e9f6ec'],[35,75,'#fbf7e1'],[75,150,'#fdeedd'],[150,99999,'#f9e1e1']]
  .forEach(([a,b,col])=>{
    if(a>=ymax) return;
    const yt=Y(Math.min(b,ymax)), yb=Y(a);
    g.fillStyle=col; g.fillRect(P.l,yt,W-P.l-P.r,yb-yt);
  });

  g.strokeStyle='#e2e2e2'; g.fillStyle='#888'; g.font='11px sans-serif'; g.textAlign='right';
  for(let k=0;k<=5;k++){
    const v=ymax*k/5, y=Y(v);
    g.beginPath(); g.moveTo(P.l,y); g.lineTo(W-P.r,y); g.stroke();
    g.fillText(Math.round(v),P.l-4,y+4);
  }

  g.textAlign='center';
  for(let k=0;k<=6;k++){
    const tt=x0+k*4*3600, dt=new Date(tt*1000);
    const lab=String(dt.getHours()).padStart(2,'0')+':'+String(dt.getMinutes()).padStart(2,'0');
    g.fillText(lab,Math.min(Math.max(X(tt),P.l+14),W-P.r-14),H-6);
  }

  g.strokeStyle='#d0502a'; g.lineWidth=2; g.lineJoin='round'; g.beginPath();
  d.forEach((p,i)=>{
    if(xs[i]<x0) return;
    const x=X(xs[i]), y=Y(p[1]);
    (i===0||xs[i-1]<x0)?g.moveTo(x,y):g.lineTo(x,y);
  });
  g.stroke();

  const lx=X(xs[xs.length-1]), ly=Y(vals[vals.length-1]);
  g.fillStyle='#d0502a'; g.beginPath(); g.arc(lx,ly,3.5,0,Math.PI*2); g.fill();
}

window.addEventListener('resize',()=>{ if(lastHist) draw(lastHist); });
loadNow(); loadHistory();
setInterval(loadNow,5000);
setInterval(loadHistory,60000);
</script>
</body></html>
)rawliteral";

void handleRoot() {
  server.send_P(200, "text/html; charset=utf-8", PAGE);
}

void handleJson() {
  String j = "{\"online\":" + String(pmsOnline() ? "true" : "false") +
             ",\"pm1\":" + String(pm1) +
             ",\"pm25\":" + String(pm25) +
             ",\"pm10\":" + String(pm10) + "}";
  server.send(200, "application/json", j);
}

void handleHistory() {
  String j = "{\"interval\":" + String(SAMPLE_SEC) + ",\"data\":[";
  int start = (histHead - histCount + HISTORY_LEN) % HISTORY_LEN;
  for (int i = 0; i < histCount; i++) {
    const Sample& s = history[(start + i) % HISTORY_LEN];
    if (i) j += ",";
    j += "[" + String(s.t) + "," + String(s.pm25) + "]";
  }
  j += "]}";
  server.send(200, "application/json", j);
}

// =====================================================
//  setup
// =====================================================
void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println();
  Serial.println("===== PM2.5 检测仪启动 =====");

  Serial2.begin(9600, SERIAL_8N1, PMS_RX_PIN, PMS_TX_PIN);

  WiFi.mode(WIFI_STA);
  WiFi.setHostname(HOST_NAME);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.print("连接 WiFi");
  unsigned long t = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t < 15000) {
    delay(500);
    Serial.print(".");
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("✅ 已连接，浏览器打开：http://");
    Serial.println(WiFi.localIP());
    if (MDNS.begin(HOST_NAME)) {
      MDNS.addService("http", "tcp", 80);
      Serial.printf("✅ 也可以用：http://%s.local\n", HOST_NAME);
    }
    configTime(8 * 3600, 0, "pool.ntp.org", "time.google.com");   // 新加坡 / 北京时间 UTC+8
  } else {
    Serial.println("⚠ WiFi 连接失败，只在串口输出数据（检查 WiFi 名、密码，是否 2.4GHz）");
  }

  server.on("/", handleRoot);
  server.on("/json", handleJson);
  server.on("/history", handleHistory);
  server.begin();

  lastSample = millis();
}

// =====================================================
//  loop
// =====================================================
unsigned long lastPrint = 0;

void loop() {
  server.handleClient();

  if (readPMS()) {
    lastPmsOk = millis();
    if (millis() > WARMUP_MS) { pmSum += pm25; pmN++; }
  }

  if (millis() - lastSample >= (unsigned long)SAMPLE_SEC * 1000) {
    lastSample = millis();
    addSample();
  }

  if (millis() - lastPrint > 2000) {
    lastPrint = millis();
    if (pmsOnline()) {
      Serial.printf("PM1.0 %u  PM2.5 %u  PM10 %u  [%s]\n", pm1, pm25, pm10, pmLevel(pm25));
    } else if (pmsBytes == 0) {
      Serial.println("PM 无数据：完全没收到 → 检查风扇转不转、TX/RX 是否接反");
    } else {
      Serial.println("PM 无数据：收到了数据但解析失败 → 检查 GND、线是否松动");
    }
  }
}
