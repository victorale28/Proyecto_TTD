/*
  Proyecto: Servicio de Prestamos - ESPOL
  Incluye:
  - AP local: "ESP32-Prestamos" (sin Internet; normal).
  - STA (2.4GHz) para NTP/SMTP (poder enviar los correos a los estudiantes).
  - NTP America/Guayaquil con reintentos, fecha "dd/mes/aaaa hh:mm" (para el historial de la web).
  - Catalogo 4 productos con 1 imagen c/u desde LittleFS (/img/p1.jpg..p4.jpg) que es una memoria del ESP32.
  - Flujo solicitudes con LCD/LEDs/botones/reed y correo (SMTP).
  - Historial estable (/admin), uploader (/upload), listado (/list), borrar (/delete).
  - Diagnostico (/net) con reintento NTP y prueba SMTP (para verificar la conexión del esp32 con la red con conexión a internet).
*/

#include <Arduino.h>        //Núcleo de Arduino: tipos básicos, setup(), loop(), pinMode, digitalWrite, Serial, etc.
#include <WiFi.h>           //Wi-Fi del ESP32: conectar en modo STA/AP, estado de red, IP.
#include <WebServer.h>      //Servidor HTTP sencillo: define rutas (server.on), atiende peticiones y responde páginas
#include <LiquidCrystal.h>  //Control de pantallas LCD 16x2/20x4 por interfaz paralela (RS, E, D4–D7): lcd.begin, lcd.print.
#include <time.h>           //Manejo de fecha/hora (estructuras time_t, tm) y uso con NTP vía configTime.
#include <FS.h>             //Interfaz genérica de sistema de archivos para Arduino (base para LittleFS/SPIFFS).
#include <LittleFS.h>       //Implementación de LittleFS en la flash del ESP32: montar, leer/escribir archivos (imágenes, etc.).
#include <ESP_Mail_Client.h>   // Mobizt

// ---------- Adelantos (para el auto-prototipado de Arduino) ----------
struct Solicitud;   // evita "Solicitud does not name a type" en prototipos auto-generados
void setupNTP();    // se usa en handleNetNtp antes de su definición

// ===== Feature flag: pagina /qr (0=OFF). La dejamos OFF.
#define ENABLE_QR_PAGE 0

// ----------------- Pines y perifericos -----------------
const int LCD_RS = 25, LCD_E = 26, LCD_D4 = 27, LCD_D5 = 33, LCD_D6 = 32, LCD_D7 = 14;
const int BTN_ROJO = 16, BTN_VERDE = 17;      // Pulsadores a GND (INPUT_PULLUP resistencias internas del ESP32)
const int LED_ROJO = 18, LED_VERDE = 19;      // Con 220 ohm
const int REED_PIN = 23;                      // Reed: cerrado=LOW, abierto=HIGH
LiquidCrystal lcd(LCD_RS, LCD_E, LCD_D4, LCD_D5, LCD_D6, LCD_D7);

// ----------------- Tiempos LCD -----------------
const unsigned long LCD_T1 = 3000;
const unsigned long LCD_T2 = 2500;
const unsigned long LCD_T3 = 2500;    //Tiempos (en milisegundos) que dura el mensaje en la LCD. (2.0 s)
const unsigned long LCD_T4 = 2500;
const unsigned long LCD_RESET_AFTER = 6000;
unsigned long g_lcdResetAtMs = 0;

// ----------------- Red: AP + STA -----------------
// AP local
const char* AP_SSID = "ESP32-Prestamos"; //Access Point (Punto de acceso).El ESP32 crea su propia red Wi-Fi (SSID/clave que tú pones en AP_SSID / AP_PASS).
const char* AP_PASS = "Prestamos1234";   //Station (Cliente Wi-Fi).El ESP32 se conecta a tu router usando STA_SSID / STA_PASS.
IPAddress ap_ip(192,168,4,1), ap_gw(192,168,4,1), ap_mask(255,255,255,0);

// STA (2.4GHz). CAMBIAR a red con internet al momento:
const char* STA_SSID = "";   // <--- Nombre de la red Wi-Fi a conectar el ESP32
const char* STA_PASS = "";     // <--- Clave en caso de que la red con internet posea

WebServer server(80);

// ----------------- NTP -----------------
//Es un protocolo que permite a tu dispositivo (ESP32) conectarse a servidores de tiempo en internet para obtener la hora actual y sincronizar su reloj interno
const char* NTP_1 = "pool.ntp.org";
const char* NTP_2 = "time.nist.gov";
const long GMT_OFFSET_SEC = -5 * 3600;
const int  DST_OFFSET_SEC = 0;
bool g_timeSynced = false;
unsigned long lastNTPAttemptMs = 0;
const unsigned long NTP_RETRY_MS = 60000;

// ----------------- Auth uploader -----------------
const char* ADMIN_USER = "";   //<-- Usuario de un admin
const char* ADMIN_PASS = ""; // <-- Clave del admin

// ----------------- SMTP (configurar) -----------------
const char* SMTP_HOST = "smtp.office365.com";
const uint16_t SMTP_PORT = 587; // STARTTLS
const char* AUTHOR_EMAIL = "";       // <--- Correo del que se emiten los correos de notificación para los estudiantes (mejor App Password)
const char* AUTHOR_PASSWORD = "";              // <--- Clave del correo definido en "AUTHOR_EMAIL" (no usar real en código)
const char* SENDER_NAME = "Servicio de Préstamos - Laboratorio ESPOL";
const char* SUBJECT_APROBADA = "Solicitud aprobada - ESPOL";
const char* SUBJECT_NEGADA   = "Solicitud negada - ESPOL";

// Logger SMTP   se refiere a un protocolo utilizado para enviar correos electrónicos
// El ESP32 puede utilizar SMTP para enviar notificaciones, alertas o cualquier tipo de información a través de correo electrónico. 
String g_smtpLog;
void smtpStatusLogger(SMTP_Status status) {
  const char* info = status.info();
  if (info) { g_smtpLog += info; g_smtpLog += "\n"; }
}

// ----------------- Catalogo ----------------- bloque para mostrar los productos en la web
struct Producto { const char* nombre; const char* descripcion; const char* img; };
Producto catalogo[4] = {
  { "Multimetro", "Instrumento para medir voltaje, corriente y resistencia. Incluye puntas.", "/img/p1.jpg" },
  { "Protoboard + Jumpers", "Tablero 830 puntos con set de cables para prototipado.", "/img/p2.jpg" },
  { "Kit Sensores", "PIR, LM35, LDR, boton, buzzer, etc. Para practicas.", "/img/p3.jpg" },
  { "Raspberry Pi Kit", "Raspberry, fuente, case y microSD. Uso supervisado.", "/img/p4.jpg" }
};

// ----------------- Modelo de datos  ----------------- Dentro de la web para solicitar el préstamo
struct Solicitud {
  uint32_t id;
  time_t   ts;        // 0 si no hay NTP 
  String   matricula;
  String   correo;
  String   nombre;
  String   producto;
  String   estado;    // pendiente | aprobada | negada | entregado
};
const size_t MAX_SOLICITUDES = 10; //tope por memoria del ESP32
Solicitud solicitudes[MAX_SOLICITUDES];
size_t writeIndex = 0;
uint32_t nextId = 1;
int ultimaIndex = -1;

// ----------------- LCD / Tiempo -----------------
void lcdPrint2Lines(const String& l1, const String& l2) {
  String a = l1.substring(0,16), b = l2.substring(0,16);
  lcd.clear(); lcd.setCursor(0,0); lcd.print(a); lcd.setCursor(0,1); lcd.print(b);
}
String center16(const String& s){ if(s.length()>=16) return s.substring(0,16); int pad=(16-s.length())/2; String o; for(int i=0;i<pad;i++) o+=" "; return o+s; }
void lcdShowWelcome(){ lcdPrint2Lines(center16("Servicio de"), center16("Prestamos ESPOL")); }
void lcdScheduleWelcomeReset(){ g_lcdResetAtMs = millis() + LCD_RESET_AFTER; }

String formatDateTime(time_t ts) {
  if (ts==0 || !g_timeSynced) return "Sin hora";
  struct tm info; localtime_r(&ts,&info);
  const char* meses[12]={"enero","febrero","marzo","abril","mayo","junio","julio","agosto","septiembre","octubre","noviembre","diciembre"};
  char buf[40]; const char* mes=(info.tm_mon>=0&&info.tm_mon<12)?meses[info.tm_mon]:"mes";
  snprintf(buf,sizeof(buf),"%02d/%s/%04d %02d:%02d",info.tm_mday,mes,1900+info.tm_year,info.tm_hour,info.tm_min);
  return String(buf);
}
time_t nowOrZero(){ return g_timeSynced? time(nullptr) : 0; }

// ----------------- Email -----------------
bool sendEmail(const String &to, const String &subject, const String &body) {
  if (WiFi.status() != WL_CONNECTED) { Serial.println("[SMTP] STA no conectada."); return false; }
  g_smtpLog = "";
  ESP_Mail_Session session;
  session.server.host_name = SMTP_HOST;
  session.server.port      = SMTP_PORT;
  session.login.email      = AUTHOR_EMAIL;
  session.login.password   = AUTHOR_PASSWORD;

  SMTP_Message message;
  message.sender.name  = SENDER_NAME;
  message.sender.email = AUTHOR_EMAIL;
  message.subject      = subject.c_str();
  message.addRecipient("Estudiante", to.c_str());
  message.text.content = body.c_str();
  message.text.charSet = "utf-8";
  message.text.transfer_encoding = Content_Transfer_Encoding::enc_7bit;

  SMTPSession smtp;
  smtp.callback(smtpStatusLogger);

  Serial.printf("[SMTP] Conectando a %s:%u como %s ...\n", SMTP_HOST, SMTP_PORT, AUTHOR_EMAIL);
  if (!smtp.connect(&session)) { Serial.println("[SMTP] No se pudo conectar."); return false; }
  if (!MailClient.sendMail(&smtp, &message)) {
    Serial.print("[SMTP] Error al enviar: "); Serial.println(smtp.errorReason());
    smtp.closeSession(); return false;
  }
  smtp.closeSession();
  Serial.println("[SMTP] Correo enviado OK.");
  if (g_smtpLog.length()) { Serial.println("[SMTP LOG]\n"+g_smtpLog); }
  return true;
}

// Prueba rapida de conexion SMTP (para /net), sirve para verificar que si se establece correctamente la conexión con la red de internet.
String smtpQuickCheck() {
  if (WiFi.status() != WL_CONNECTED) return "STA no conectada";
  g_smtpLog = "";
  ESP_Mail_Session session;
  session.server.host_name = SMTP_HOST;
  session.server.port      = SMTP_PORT;
  session.login.email      = AUTHOR_EMAIL;
  session.login.password   = AUTHOR_PASSWORD;
  SMTPSession smtp;
  smtp.callback(smtpStatusLogger);
  if (!smtp.connect(&session)) return "Fallo conexion SMTP (host/puerto/credenciales/red)";
  smtp.closeSession();
  return "OK (conexion SMTP exitosa)\n" + g_smtpLog;
}

String makeBodyAprobada(const Solicitud & s){
  return String("Hola ")+s.nombre+
         ",\n\nSu solicitud ha sido aprobada. Por favor acerquese al laboratorio para retirarlo.\n\n"+
         String("Articulo: ")+s.producto+"\nMatricula: "+s.matricula+
         "\nID: "+s.id+"\nFecha: "+formatDateTime(s.ts)+"\n\nBuen dia.";
}
String makeBodyNegada(const Solicitud & s){
  return String("Hola ")+s.nombre+
         ",\n\nSu solicitud ha sido negada por falta de disponibilidad del articulo.\n\n"+
         String("Articulo: ")+s.producto+"\nMatricula: "+s.matricula+
         "\nID: "+s.id+"\nFecha: "+formatDateTime(s.ts)+"\n\nBuen dia.";
}

// ----------------- HTML helpers -----------------
String htmlEscape(const String& in){
  String s; s.reserve(in.length()+10);
  for (size_t i=0;i<in.length();++i){
    char c=in[i];
    if(c=='&') s+="&amp;";
    else if(c=='<') s+="&lt;";
    else if(c=='>') s+="&gt;";
    else if(c=='\"') s+="&quot;";
    else if(c=='\'') s+="&#39;";
    else s+=c;
  }
  return s;
}

// ----------------- HTML / UI -----------------
String htmlHeader(const String& title="Servicio de Prestamos - ESPOL"){
  String h;
  h+="<!DOCTYPE html><html lang='es'><head><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'>";
  h+="<title>"+title+"</title><style>";
  h+="body{font-family:system-ui,Arial,sans-serif;margin:0;padding:24px;background:#f6f7fb}";
  h+=".wrap{max-width:1000px;margin:0 auto;background:#fff;border-radius:12px;padding:18px;box-shadow:0 8px 24px rgba(0,0,0,.08)}";
  h+="h1{font-size:1.1rem;text-align:center;margin:4px 0 14px 0}";
  h+=".grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(220px,1fr));gap:12px;margin-top:8px}";
  h+=".card{border:1px solid #eee;border-radius:10px;padding:12px;display:flex;flex-direction:column;gap:8px;align-items:center;text-align:center}";
  h+=".thumb{width:100%;max-height:200px;object-fit:cover;border-radius:8px;border:1px solid #eee;display:block;margin:0 auto}";
  h+=".muted{color:#666}";
  h+="a.button,button{display:inline-block;padding:8px 12px;border-radius:8px;border:1px solid #ddd;background:#fafafa;cursor:pointer;text-decoration:none}";
  h+="a.button:hover,button:hover{background:#f0f0f0}";
  h+="label{display:block;margin:.3rem 0 .15rem 0} input{width:100%;padding:8px;border:1px solid #ddd;border-radius:8px}";
  h+=".ok{background:#e8f5e9;border:1px solid #c8e6c9;color:#256029;padding:.8rem;border-radius:8px;margin:.5rem 0}";
  h+=".err{background:#ffebee;border:1px solid #ffcdd2;color:#b71c1c;padding:.8rem;border-radius:8px;margin:.5rem 0}";
  h+=".topnav{display:flex;gap:8px;margin-bottom:8px}.pill{display:inline-block;padding:2px 8px;border-radius:999px;background:#f0f0f0;border:1px solid #e5e5e5;font-size:.85rem}";
  h+=".table-wrap{width:100%;overflow:auto;border:1px solid #eee;border-radius:10px}";
  h+="table{border-collapse:separate;border-spacing:0;table-layout:fixed;min-width:820px;width:100%}";
  h+="thead th{position:sticky;top:0;background:#fafafa;border-bottom:1px solid #ddd;z-index:1}";
  h+="th,td{border-right:1px solid #eee;padding:8px 10px;vertical-align:top;word-break:break-word;font-size:.92rem}";
  h+="tr:nth-child(even){background:#fcfcfd}";
  h+="th:last-child,td:last-child{border-right:none}";
  h+=".nowrap{white-space:nowrap}.mono{font-family:ui-monospace,Consolas,monospace}";
  h+=".td-id{width:64px}.td-fecha{width:180px}.td-mat{width:140px}.td-prod{width:180px}.td-estado{width:120px}";
  h+="</style></head><body><div class='wrap'><div class='topnav'>";
  h+="<a class='button' href='/'>Inicio</a>";
  h+="<a class='button' href='/admin'>Historial</a>";
  h+="<a class='button' href='/status'>Status</a>";
  h+="<a class='button' href='/net'>Red</a>";
  h+="</div>";
  h+="<h1>Servicio de Prestamos para estudiantes - Espol</h1>";
  return h;
}
String htmlFooter(){ return "</div></body></html>"; }

// Home  Para el diseño de la web, con respecto a la pestaña de inicio.
String pageHome(){
  String s = htmlHeader();
  IPAddress staIP = WiFi.localIP();
  if (staIP[0]!=0) s += String("<div class='ok'>IP en la red del laboratorio: <span class='pill'>http://")+staIP.toString()+"/</span></div>";
  else s += "<div class='ok'>Funcionando por AP. Imagenes offline desde LittleFS.</div>";
  s += "<p class='muted'>Seleccione un producto para ver su descripcion y solicitar el prestamo.</p><div class='grid'>";
  for(int i=0;i<4;i++){
    s += "<div class='card'>";
    s += String("<img class='thumb' src='")+catalogo[i].img+"' alt='producto'>";
    s += String("<b>")+catalogo[i].nombre+"</b>";
    s += "<p class='muted'>"+String(catalogo[i].descripcion)+"</p>";
    s += String("<a class='button' href='/producto?i=")+i+"'>Seleccionar</a>";
    s += "</div>";
  }
  s += "</div>"+htmlFooter();
  return s;
}

// Detalle + formulario
String pageProducto(int idx, const String& errMsg=""){
  if(idx<0||idx>=4) idx=0;
  String s = htmlHeader(String("Solicitar - ")+catalogo[idx].nombre);
  s += String("<img class='thumb' style='max-height:280px' src='")+catalogo[idx].img+"' alt='producto'>";
  if(errMsg.length()) s += String("<div class='err'>")+errMsg+"</div>";
  s += "<div class='card'><b>Producto seleccionado:</b> "+String(catalogo[idx].nombre)+"<br><p class='muted'>"+String(catalogo[idx].descripcion)+"</p></div>";
  s += "<form method='POST' action='/solicitar'>";
  s += String("<input type='hidden' name='i' value='")+idx+"'>";
  s += "<label># de matricula</label><input name='matricula' required pattern='[0-9]+' placeholder='Ej: 2023123456'>";
  s += "<label>Correo institucional (@espol.edu.ec)</label><input type='email' name='correo' required placeholder='usuario@espol.edu.ec'>";
  s += "<label>Nombre completo</label><input name='nombre' required placeholder='Nombre Apellido'><br>";
  s += "<button type='submit'>Solicitar</button></form>";
  s += htmlFooter();
  return s;
}

// Confirmacion
String pageConfirm(const Solicitud& sol){
  String s = htmlHeader("Solicitud recibida");
  s += "<div class='ok'><b>Solicitud recibida.</b> Estado: <b id='estado'>en revision</b>.</div>";
  s += "<div class='card'>";
  s += String("<b>Nombre:</b> ")+htmlEscape(sol.nombre)+"<br>";
  s += String("<b>Matricula:</b> ")+htmlEscape(sol.matricula)+"<br>";
  s += String("<b>Producto:</b> ")+htmlEscape(sol.producto)+"<br>";
  s += String("<b>ID:</b> ")+sol.id+"<br>";
  s += String("<b>Fecha:</b> ")+formatDateTime(sol.ts)+"<br>";
  s += "</div>";
  s += "<p class='muted'>Este estado se actualizara automaticamente cuando el laboratorio decida.</p>";
  s += "<script>const sid="+String(sol.id)+";async function poll(){try{const r=await fetch('/api/estado?id='+sid);if(r.ok){const j=await r.json();document.getElementById('estado').textContent=j.estado;}}catch(e){}} setInterval(poll,1500); poll();</script>";
  s += htmlFooter();
  return s;
}

// Historial  -  Apartado en la web para llevar un registro.
String pageAdmin(){
  String s = htmlHeader("Historial de solicitudes");
  s += "<div class='card'><b>Historial (RAM, max 10 registros)</b></div>";
  s += "<div class='table-wrap'><table>";
  s += "<thead><tr>"
       "<th class='td-id'>ID</th>"
       "<th class='td-fecha'>Fecha</th>"
       "<th>Nombre</th>"
       "<th class='td-mat'>Matricula</th>"
       "<th>Correo</th>"
       "<th class='td-prod'>Producto</th>"
       "<th class='td-estado'>Estado</th>"
       "</tr></thead><tbody>";
  for(size_t k=0;k<MAX_SOLICITUDES;k++){
    const Solicitud& r = solicitudes[k];
    if(r.id==0) continue;
    s += "<tr>";
    s += String("<td class='mono nowrap'>")+r.id+"</td>";
    s += String("<td class='nowrap'>")+formatDateTime(r.ts)+"</td>";
    s += String("<td>")+htmlEscape(r.nombre)+"</td>";
    s += String("<td class='mono'>")+htmlEscape(r.matricula)+"</td>";
    s += String("<td>")+htmlEscape(r.correo)+"</td>";
    s += String("<td>")+htmlEscape(r.producto)+"</td>";
    s += String("<td class='mono'>")+htmlEscape(r.estado)+"</td>";
    s += "</tr>";
  }
  s += "</tbody></table></div>";
  s += htmlFooter();
  return s;
}

// Pagina de diagnostico
String pageNet(){
  String s = htmlHeader("Diagnostico de red");
  wl_status_t st = WiFi.status();
  IPAddress staIP = WiFi.localIP();
  s += "<div class='card'><b>AP (local)</b><br>SSID: <b>"+String(AP_SSID)+"</b><br>IP: <b>"+ap_ip.toString()+"</b></div>";
  s += "<div class='card'><b>STA (Internet)</b><br>";
  s += "Estado: <b>"+String(st==WL_CONNECTED?"Conectado":"No conectado")+"</b><br>";
  if (st==WL_CONNECTED) {
    s += "SSID: <b>"+WiFi.SSID()+"</b><br>";
    s += "IP: <b>"+staIP.toString()+"</b> | GW: <b>"+WiFi.gatewayIP().toString()+"</b> | DNS: <b>"+WiFi.dnsIP().toString()+"</b><br>";
    s += "RSSI: <b>"+String(WiFi.RSSI())+" dBm</b><br>";
  } else {
    s += "Consejo: usa red 2.4GHz (no 5GHz), revisa clave y evita portal cautivo.<br>";
  }
  s += "</div>";
  s += "<div class='card'><b>NTP</b><br>Hora: <b>"+formatDateTime(time(nullptr))+"</b><br>Sincronizado: <b>"+String(g_timeSynced?"si":"no")+"</b><br>";
  s += "<a class='button' href='/net/ntp-resync'>Reintentar NTP</a></div>";
  s += "<div class='card'><b>SMTP</b><br>Host: <b>"+String(SMTP_HOST)+":"+String(SMTP_PORT)+"</b><br>Usuario: <b>"+String(AUTHOR_EMAIL)+"</b><br>";
  s += "<a class='button' href='/net/smtp-check'>Probar conexion SMTP</a></div>";
  s += htmlFooter();
  return s;
}

// ----------------- File serving -----------------
String contentTypeFor(const String& p){
  if(p.endsWith(".htm")||p.endsWith(".html")) return "text/html";
  if(p.endsWith(".css")) return "text/css";
  if(p.endsWith(".js")) return "application/javascript";
  if(p.endsWith(".png")) return "image/png";
  if(p.endsWith(".jpg")||p.endsWith(".jpeg")) return "image/jpeg";
  if(p.endsWith(".gif")) return "image/gif";
  if(p.endsWith(".svg")) return "image/svg+xml";
  if(p.endsWith(".ico")) return "image/x-icon";
  return "application/octet-stream";
}
bool tryServeFile(const String& path){
  if(!LittleFS.exists(path)) return false;
  File f = LittleFS.open(path,"r"); if(!f) return false;
  String ctype = contentTypeFor(path); server.streamFile(f, ctype); f.close(); return true;
}

// ----------------- Handlers HTTP ----------------- Apartado para procesar o administrar las solicitudes de la web
void handleRoot(){ server.send(200,"text/html",pageHome()); }
void handleStatus(){ server.send(200,"text/plain","OK"); }
void handleProducto(){ int idx=server.hasArg("i")?server.arg("i").toInt():0; server.send(200,"text/html",pageProducto(idx)); }
void handleAdmin(){ server.send(200,"text/html",pageAdmin()); }
void handleNet(){ server.send(200,"text/html",pageNet()); }

// Aux: termina con sufijo, case-insensitive
bool endsWithCaseInsensitive(const String& t, const String& sfx){
  if(t.length()<sfx.length()) return false; String a=t; a.toLowerCase(); String b=sfx; b.toLowerCase(); return a.endsWith(b);
}

// POST /solicitar
void handleSolicitar(){
  if(server.method()!=HTTP_POST){ server.send(405,"text/plain","Metodo no permitido"); return; }
  if(!server.hasArg("i")||!server.hasArg("matricula")||!server.hasArg("correo")||!server.hasArg("nombre")){
    server.send(400,"text/plain","Faltan campos"); return;
  }
  int idx = server.arg("i").toInt(); if(idx<0||idx>=4) idx=0;

  String matricula = server.arg("matricula");
  for(size_t i=0;i<matricula.length();++i) if(!isDigit(matricula[i])){
    server.send(200,"text/html",pageProducto(idx,"La matricula debe contener solo digitos.")); return; }

  String correo = server.arg("correo");
  if(!endsWithCaseInsensitive(correo,"@espol.edu.ec")){
    server.send(200,"text/html",pageProducto(idx,"El correo debe ser institucional (@espol.edu.ec).")); return; }

  String nombre = server.arg("nombre"); nombre.trim();
  if(nombre.length()<2){ server.send(200,"text/html",pageProducto(idx,"Ingrese un nombre valido.")); return; }

  // Guardar
  size_t pos = writeIndex % MAX_SOLICITUDES;
  Solicitud& s = solicitudes[pos];
  s.id=nextId++; s.ts=nowOrZero(); s.matricula=matricula; s.correo=correo; s.nombre=nombre; s.producto=catalogo[idx].nombre; s.estado="pendiente";
  ultimaIndex = (int)pos; writeIndex++;

  // LCD - Para mostrar mensajes en la pantalla LCD
  lcdPrint2Lines(center16("solicitud de:"), center16("")); delay(LCD_T1);
  lcdPrint2Lines("solicitud de:", (nombre)); delay(LCD_T1);
  lcdPrint2Lines("Articulo:", s.producto);                 delay(LCD_T2);
  lcdPrint2Lines("Fecha:", formatDateTime(s.ts));           delay(LCD_T3);
  lcdPrint2Lines("Estado:", "En revision");                 delay(LCD_T4);

  // Pagina de confirmacion
  server.send(200,"text/html",pageConfirm(s));

  // Log
  Serial.println("=== Nueva solicitud ===");
  Serial.printf("ID:%lu\n",(unsigned long)s.id);
  Serial.printf("Nombre:%s\n",s.nombre.c_str());
  Serial.printf("Matricula:%s\n",s.matricula.c_str());
  Serial.printf("Correo:%s\n",s.correo.c_str());
  Serial.printf("Producto:%s\n",s.producto.c_str());
  Serial.printf("Fecha:%s\n",formatDateTime(s.ts).c_str());
  Serial.println("Estado: pendiente");
}

// API estado-mantiene la información del estado o la sesión del servidor relacionada con cada cliente o secuencia de solicitudes.
void handleApiEstado(){
  if(!server.hasArg("id")){ server.send(400,"application/json","{\"error\":\"falta id\"}"); return; }
  uint32_t idq = (uint32_t) server.arg("id").toInt();
  for(size_t k=0;k<MAX_SOLICITUDES;k++){
    if(solicitudes[k].id==idq){
      String j = String("{\"id\":")+idq+",\"estado\":\""+solicitudes[k].estado+"\"}";
      server.send(200,"application/json",j); return;
    }
  }
  server.send(404,"application/json","{\"error\":\"no encontrado\"}");
}

// net acciones
void handleNetNtp(); // prototipo para server.on
void handleNetSmtp(){ String r = smtpQuickCheck(); server.send(200,"text/plain", r); }
void handleNetNtp(){ setupNTP(); server.sendHeader("Location","/net"); server.send(302,"text/plain","Reintentando NTP"); }

// ----------------- Uploader protegido ----------------- Permitir cargar imagenes en el ESP32 para cada producto
bool ensureAuth(){ if(!server.authenticate(ADMIN_USER,ADMIN_PASS)){ server.requestAuthentication(); return false; } return true; }
File g_uploadFile;

void handleUploadPage(){
  if(!ensureAuth()) return;
  String html = htmlHeader("Subir archivo (LittleFS)");
  html += "<div class='card'><b>Subir a /img/ (imagenes) o raiz</b><br>";
  html += "<form method='POST' action='/upload' enctype='multipart/form-data'>";
  html += "<input type='file' name='upfile' accept='.jpg,.jpeg,.png,.gif,.svg,.js' required>";
  html += "<br><small class='muted'>Recomendado imagen: JPG <=100 KB, 600x360 o 800x450</small><br><br>";
  html += "<button type='submit'>Subir</button></form></div>";
  html += "<div class='card'><a class='button' href='/list'>Ver archivos</a></div>";
  html += htmlFooter();
  server.send(200,"text/html",html);
}

void handleUploadData(){
  if(!ensureAuth()) return;
  HTTPUpload& up = server.upload();
  if(up.status==UPLOAD_FILE_START){
    String filename = up.filename;
    if(!filename.startsWith("/")) filename="/"+filename;
    if(!filename.endsWith(".js") && !filename.startsWith("/img/")){
      if(filename.endsWith(".jpg")||filename.endsWith(".jpeg")||filename.endsWith(".png")||filename.endsWith(".gif")||filename.endsWith(".svg")){
        filename = String("/img/")+ filename.substring(filename.lastIndexOf('/')+1);
      }
    }
    Serial.printf("Subiendo: %s\n", filename.c_str());
    g_uploadFile = LittleFS.open(filename,"w");
  } else if(up.status==UPLOAD_FILE_WRITE){
    if(g_uploadFile) g_uploadFile.write(up.buf, up.currentSize);
  } else if(up.status==UPLOAD_FILE_END){
    if(g_uploadFile){ g_uploadFile.close(); Serial.printf("Subida completa (%u bytes)\n", up.totalSize); }
    else Serial.println("Error: no se pudo abrir para escribir.");
  }
}

void handleUploadPost(){
  if(!ensureAuth()) return;
  String html = htmlHeader("Subir archivo");
  html += "<div class='ok'>Archivo subido.</div>";
  html += "<div class='card'><a class='button' href='/upload'>Subir otro</a> <a class='button' href='/list'>Ver archivos</a></div>";
  html += htmlFooter();
  server.send(200,"text/html",html);
}

void handleList(){
  if(!ensureAuth()) return;
  String html = htmlHeader("Archivos en LittleFS");
  html += "<div class='card'><b>Contenido</b></div><div class='table-wrap'><table>";
  html += "<thead><tr><th>Archivo</th><th>Tamano</th><th>Preview</th><th>Accion</th></tr></thead><tbody>";
  File root = LittleFS.open("/");
  if(!root){ html += "<tr><td colspan='4'>No se pudo abrir raiz.</td></tr>"; }
  else {
    File f = root.openNextFile();
    while(f){
      String name=f.name(); size_t size=f.size(); bool isDir=f.isDirectory();
      html += "<tr>";
      html += "<td>"+htmlEscape(name)+"</td>";
      html += "<td class='mono nowrap'>"+String(size)+(isDir?" (dir)":"")+"</td>";
      if(name.endsWith(".jpg")||name.endsWith(".jpeg")||name.endsWith(".png")||name.endsWith(".gif")||name.endsWith(".svg"))
        html += String("<td><img src='")+htmlEscape(name)+"' style='max-width:160px;max-height:100px;border:1px solid #ddd;border-radius:6px'></td>";
      else html += "<td>-</td>";
      html += String("<td><form method='POST' action='/delete'><input type='hidden' name='path' value='")+htmlEscape(name)+"'><button type='submit'>Borrar</button></form></td>";
      html += "</tr>";
      f = root.openNextFile();
    }
    if (LittleFS.exists("/img")) {
      File dir = LittleFS.open("/img");
      File g = dir.openNextFile();
      while(g){
        String name=g.name(); size_t size=g.size();
        html += "<tr>";
        html += "<td>"+htmlEscape(name)+"</td>";
        html += "<td class='mono nowrap'>"+String(size)+"</td>";
        if(name.endsWith(".jpg")||name.endsWith(".jpeg")||name.endsWith(".png")||name.endsWith(".gif")||name.endsWith(".svg"))
          html += String("<td><img src='")+htmlEscape(name)+"' style='max-width:160px;max-height:100px;border:1px solid #ddd;border-radius:6px'></td>";
        else html += "<td>-</td>";
        html += String("<td><form method='POST' action='/delete'><input type='hidden' name='path' value='")+htmlEscape(name)+"'><button type='submit'>Borrar</button></form></td>";
        html += "</tr>";
        g = dir.openNextFile();
      }
    }
  }
  html += "</tbody></table></div>"+htmlFooter();
  server.send(200,"text/html",html);
}

void handleDelete(){
  if(!ensureAuth()) return;
  if(!server.hasArg("path")){ server.send(400,"text/plain","Falta 'path'"); return; }
  String path=server.arg("path");
  if(!LittleFS.exists(path)){ server.send(404,"text/plain","No existe"); return; }
  LittleFS.remove(path);
  server.sendHeader("Location","/list"); server.send(302,"text/plain","Borrado");
}

// ----------------- Red/NTP -----------------
void setupNTP(){
  if(WiFi.status()!=WL_CONNECTED){ g_timeSynced=false; return; }
  configTime(GMT_OFFSET_SEC, DST_OFFSET_SEC, NTP_1, NTP_2);
  Serial.println("Sincronizando NTP...");
  struct tm info; g_timeSynced=false;
  for(int i=0;i<12;i++){ if(getLocalTime(&info,700)){ g_timeSynced=true; break; } }
  if(g_timeSynced){ char buf[32]; strftime(buf,sizeof(buf),"%d/%m/%Y %H:%M",&info); Serial.printf("NTP OK: %s\n", buf); }
  else Serial.println("NTP no disponible (mostrar 'Sin hora').");
}

void tryResyncNTP(){
  if(g_timeSynced) return;
  if(WiFi.status()!=WL_CONNECTED) return;
  unsigned long nowMs = millis();
  if(nowMs - lastNTPAttemptMs < NTP_RETRY_MS) return;
  lastNTPAttemptMs = nowMs;
  Serial.println("[NTP] Reintentando sincronizacion...");
  setupNTP();
}

void startAPAndHTTP(){
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAPConfig(ap_ip,ap_gw,ap_mask);
  WiFi.softAP(AP_SSID,AP_PASS);
  delay(100);

  // Portal
  server.on("/", HTTP_GET, handleRoot);
  server.on("/status", HTTP_GET, handleStatus);
  server.on("/producto", HTTP_GET, handleProducto);
  server.on("/solicitar", HTTP_POST, handleSolicitar);
  server.on("/admin", HTTP_GET, handleAdmin);
  server.on("/api/estado", HTTP_GET, handleApiEstado);
  server.on("/net", HTTP_GET, handleNet);
  server.on("/net/ntp-resync", HTTP_GET, handleNetNtp);
  server.on("/net/smtp-check", HTTP_GET, handleNetSmtp);

  // Uploader protegido
  server.on("/upload", HTTP_GET, handleUploadPage);
  server.on("/upload", HTTP_POST, handleUploadPost, handleUploadData);
  server.on("/list",   HTTP_GET, handleList);
  server.on("/delete", HTTP_POST, handleDelete);

  // Archivos estaticos
  server.onNotFound([](){
    String uri = server.uri();
    if(tryServeFile(uri)) return;
    server.send(404,"text/plain","No encontrado");
  });

  server.begin();
  Serial.printf("\nAP listo. SSID:%s  Pass:%s  URL: http://%s\n", AP_SSID, AP_PASS, ap_ip.toString().c_str());
  Serial.println("Uploader: /upload (Basic Auth)");
}

void connectSTA(){
  if(strlen(STA_SSID)==0||strlen(STA_PASS)==0){ Serial.println("(!) STA sin credenciales."); return; }
  Serial.printf("Conectando STA (2.4GHz) a %s ...\n", STA_SSID);
  WiFi.begin(STA_SSID, STA_PASS);
  unsigned long t0=millis();
  while(WiFi.status()!=WL_CONNECTED && millis()-t0<15000UL){ delay(200); server.handleClient(); Serial.print("."); }
  Serial.println();
  if(WiFi.status()==WL_CONNECTED) Serial.printf("STA OK. IP: %s  RSSI: %d dBm\n", WiFi.localIP().toString().c_str(), WiFi.RSSI());
  else Serial.println("STA timeout. Continuando solo con AP.");
}

// ----------------- Botones/Reed/LEDs -----------------
bool prevRed = HIGH, prevGreen = HIGH, prevReed = HIGH;
unsigned long lastBtnMs = 0;
const unsigned long DEBOUNCE_MS = 250;

void procesaBotonesYReed(){
  bool redNow   = digitalRead(BTN_ROJO);
  bool greenNow = digitalRead(BTN_VERDE);
  bool reedNow  = digitalRead(REED_PIN);
  unsigned long nowMs = millis();

  // LED ROJO: ON mientras boton rojo presionado (LOW), OFF al soltar
  digitalWrite(LED_ROJO, (redNow==LOW) ? HIGH : LOW);

  // NEGAR
  if(prevRed==HIGH && redNow==LOW && (nowMs-lastBtnMs>DEBOUNCE_MS)){
    lastBtnMs = nowMs;
    if(ultimaIndex!=-1){
      Solicitud& s = solicitudes[ultimaIndex];
      if(s.id!=0 && s.estado=="pendiente"){
        s.estado = "negada";
        lcdPrint2Lines("Solicitud","Negada");
        lcdScheduleWelcomeReset();
        digitalWrite(LED_VERDE, LOW);
        Serial.printf("Solicitud %lu NEGADA\n",(unsigned long)s.id);
        sendEmail(s.correo, SUBJECT_NEGADA, makeBodyNegada(s));
      }
    }
  }

  // APROBAR
  if(prevGreen==HIGH && greenNow==LOW && (nowMs-lastBtnMs>DEBOUNCE_MS)){
    lastBtnMs = nowMs;
    if(ultimaIndex!=-1){
      Solicitud& s = solicitudes[ultimaIndex];
      if(s.id!=0 && s.estado=="pendiente"){
        s.estado = "aprobada";
        lcdPrint2Lines("Solicitud","Aprobada");
        digitalWrite(LED_VERDE, HIGH); // se apaga con reed abierto
        Serial.printf("Solicitud %lu APROBADA\n",(unsigned long)s.id);
        sendEmail(s.correo, SUBJECT_APROBADA, makeBodyAprobada(s));
      }
    }
  }

  // ENTREGA: reed LOW->HIGH
  if(prevReed==LOW && reedNow==HIGH){
    if(ultimaIndex!=-1){
      Solicitud& s = solicitudes[ultimaIndex];
      if(s.id!=0 && s.estado=="aprobada"){
        s.estado = "entregado";
        lcdPrint2Lines("Prestamo","realizado OK");
        digitalWrite(LED_VERDE, LOW);
        lcdScheduleWelcomeReset();
        Serial.printf("Solicitud %lu ENTREGADA\n",(unsigned long)s.id);
      }
    }
  }

  // Actualizar previos
  prevRed = redNow; prevGreen = greenNow; prevReed = reedNow;

  // Retorno a bienvenida
  if(g_lcdResetAtMs && millis() >= g_lcdResetAtMs){
    g_lcdResetAtMs = 0;
    lcdShowWelcome();
  }
}

// ----------------- Setup / Loop -----------------
void setup(){
  Serial.begin(115200); delay(200);

  lcd.begin(16,2); lcd.clear(); lcdShowWelcome();

  pinMode(BTN_ROJO, INPUT_PULLUP);
  pinMode(BTN_VERDE, INPUT_PULLUP);
  pinMode(LED_ROJO, OUTPUT);  digitalWrite(LED_ROJO, LOW);
  pinMode(LED_VERDE, OUTPUT); digitalWrite(LED_VERDE, LOW);
  pinMode(REED_PIN, INPUT_PULLUP);
  prevReed = digitalRead(REED_PIN);

  if(!LittleFS.begin(true)){ Serial.println("Error montando LittleFS"); }
  else { if(!LittleFS.exists("/img")) LittleFS.mkdir("/img"); }

  startAPAndHTTP();
  connectSTA();     // 2.4GHz
  setupNTP();
}

void loop(){
  server.handleClient();
  procesaBotonesYReed();
  tryResyncNTP();
  delay(5);
}