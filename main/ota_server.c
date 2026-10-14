#include "ota_server.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "display_settings.h"
#include "lighting_settings.h"
#include "matter_display.h"
#include "animation_test.h"
#include "price_test.h"
#include "password_display.h"
#include "service.h"
#include "upgrade_password.h"
#include "esp_app_format.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define OTA_PASSWORD_MAX_LENGTH UPGRADE_PASSWORD_MAX_LENGTH
#define OTA_RECEIVE_BUFFER_SIZE 2048
#define OTA_REBOOT_DELAY_MS 1500

static const char *TAG = "display_ota";
static httpd_handle_t s_server;
static char s_password[OTA_PASSWORD_MAX_LENGTH + 1];
static size_t s_password_length;
/* URI handlers run sequentially on the HTTP server's own task. */
static bool s_restart_pending;
static bool authorized(httpd_req_t *request);

static const char PAGE_START[] =
    "<!doctype html><html lang=\"en\"><meta charset=\"utf-8\">"
    "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
    "<title>Angst</title><style>"
    ":root{color-scheme:light dark;font-family:system-ui,sans-serif;color:#202633;"
    "background:#f2f4f7}*{box-sizing:border-box}body{margin:0;padding:48px 20px}"
    "main{max-width:560px;margin:auto;background:#fff;border:1px solid #dfe3e9;"
    "border-radius:18px;padding:32px;box-shadow:0 12px 40px #2430440c}"
    "h1{font-size:40px;letter-spacing:-.03em;margin:0 0 12px}"
    "h2{font-size:19px;margin:28px 0 12px}p{line-height:1.6;color:#58677e}"
    "label{display:block;font-size:14px;font-weight:600;margin:24px 0 8px}"
    "input,select{width:100%;padding:12px;border:1px solid #ccd3df;border-radius:8px;"
    "font:inherit;background:#fff;color:inherit}input[type=file]{font-size:14px}"
    "button{width:100%;margin-top:28px;border:0;border-radius:9px;padding:13px;"
    "background:#2855dd;color:white;font:600 15px system-ui;cursor:pointer}"
    "button:disabled{opacity:.5;cursor:default}#status,#settings-status,#password-status,#price-url-status,#show-password-status,#price-test-status,#animation-test-status,#lighting-gpios-status{min-height:24px;"
    "font-size:14px;margin-bottom:0}"
    ".toggle{display:flex;align-items:center;gap:12px;margin:20px 0 0;font-size:16px;cursor:pointer}"
    ".toggle input{width:20px;height:20px;margin:0;accent-color:#2855dd}"
    "section+section{margin-top:28px;padding-top:8px;border-top:1px solid #dfe3e9}"
    ".toggle input:disabled+span{opacity:.5}"
    ".test-actions{display:flex;flex-wrap:wrap;gap:12px}.test-actions button{flex:1 1 190px;margin-top:0}"
    "details{margin-top:28px}summary{font-weight:600;cursor:pointer}"
    "@media(prefers-color-scheme:dark){:root{background:#111722;color:#e8edf5}"
    "main{background:#1b2431;border-color:#344155}p{color:#aebacf}"
    "input,select{background:#111722;border-color:#45536b}section+section{border-color:#344155}}"
    "</style><main><h1>Angst</h1>"
    "<section aria-labelledby=\"settings-heading\">"
    "<h2 id=\"settings-heading\">Display settings</h2>"
    "<label class=\"toggle\" for=\"animation\">"
    "<input id=\"animation\" type=\"checkbox\" disabled><span>Animation</span></label>"
    "<label class=\"toggle\" for=\"stick-figure\">"
    "<input id=\"stick-figure\" type=\"checkbox\" disabled><span>Stick figure animation</span></label>"
    "<p id=\"settings-status\" role=\"status\" aria-live=\"polite\">Loading settings…</p></section>"
    "<section aria-labelledby=\"price-test-heading\"><h2 id=\"price-test-heading\">Price animation test</h2>"
    "<div class=\"test-actions\"><button id=\"test-price-increase\" type=\"button\">Test price increase</button>"
    "<button id=\"test-price-decrease\" type=\"button\">Test price decrease</button></div>"
    "<p id=\"price-test-status\" role=\"status\" aria-live=\"polite\"></p></section>"
    "<section aria-labelledby=\"animation-test-heading\"><h2 id=\"animation-test-heading\">Stick figure animations</h2>"
    "<p>Choose any scene to play on the display.</p><label for=\"animation-choice\">Animation</label>"
    "<select id=\"animation-choice\" disabled></select>"
    "<button id=\"run-animation\" type=\"button\" disabled>Run animation</button>"
    "<p id=\"animation-test-status\" role=\"status\" aria-live=\"polite\">Loading animations…</p></section>"
    "<section aria-labelledby=\"price-heading\"><h2 id=\"price-heading\">Price updates</h2>"
    "<form id=\"price-url-form\"><label for=\"price-url\">Price URL</label>"
    "<input id=\"price-url\" type=\"url\" maxlength=\"1024\" placeholder=\"https://example.com/price\" disabled>"
    "<label for=\"price-url-password\">Password</label>"
    "<input id=\"price-url-password\" type=\"password\" autocomplete=\"current-password\" maxlength=\"128\" required disabled>"
    "<button id=\"save-price-url\" type=\"submit\" disabled>Save price URL</button>"
    "<p id=\"price-url-status\" role=\"status\" aria-live=\"polite\">Loading price URL…</p></form></section>"
    "<section aria-labelledby=\"firmware-heading\"><h2 id=\"firmware-heading\">Firmware upgrade</h2>"
    "<form id=\"upload\" data-enabled=\"";

static const char PAGE_END[] =
    "\"><label for=\"firmware\">Firmware file</label>"
    "<input id=\"firmware\" name=\"firmware\" type=\"file\" accept=\".bin\" required>"
    "<label for=\"password\">Password</label>"
    "<input id=\"password\" type=\"password\" autocomplete=\"off\" maxlength=\"128\" required>"
    "<button id=\"submit\" type=\"submit\">Upload firmware</button>"
    "<p id=\"status\" role=\"status\" aria-live=\"polite\"></p></form></section>"
    "<section aria-labelledby=\"lighting-heading\"><h2 id=\"lighting-heading\">LED lighting GPIOs</h2>"
    "<form id=\"lighting-gpios-form\"><label for=\"lighting-gpios\">GPIO pins</label>"
    "<input id=\"lighting-gpios\" type=\"text\" maxlength=\"147\" placeholder=\"45,46\" required disabled>"
    "<p id=\"lighting-gpios-options\"></p><label for=\"lighting-gpios-password\">Password</label>"
    "<input id=\"lighting-gpios-password\" type=\"password\" autocomplete=\"current-password\" maxlength=\"128\" required disabled>"
    "<button id=\"save-lighting-gpios\" type=\"submit\" disabled>Save GPIOs</button>"
    "<p id=\"lighting-gpios-status\" role=\"status\" aria-live=\"polite\">Loading GPIOs…</p></form></section>"
    "<section aria-label=\"Change password\">"
    "<button id=\"show-password\" type=\"button\">Show password on display</button>"
    "<p id=\"show-password-status\" role=\"status\" aria-live=\"polite\"></p>"
    "<details><summary>Change password</summary><form id=\"change-password\">"
    "<label for=\"current-password\">Current password</label>"
    "<input id=\"current-password\" type=\"password\" autocomplete=\"current-password\" maxlength=\"128\" required>"
    "<label for=\"new-password\">New password</label>"
    "<input id=\"new-password\" type=\"password\" autocomplete=\"new-password\" maxlength=\"128\" required>"
    "<label for=\"confirm-password\">Confirm new password</label>"
    "<input id=\"confirm-password\" type=\"password\" autocomplete=\"new-password\" maxlength=\"128\" required>"
    "<button id=\"save-password\" type=\"submit\">Change password</button>"
    "<p id=\"password-status\" role=\"status\" aria-live=\"polite\"></p>"
    "</form></details></section></main>"
    "<script>const animation=document.getElementById('animation'),"
    "stickFigure=document.getElementById('stick-figure'),"
    "settingsStatus=document.getElementById('settings-status');"
    "let savedSettings={animation_enabled:false,stick_figure_enabled:false},settingsLoaded=false,settingsBusy=false;"
    "const lightingGpiosForm=document.getElementById('lighting-gpios-form'),lightingGpios=document.getElementById('lighting-gpios'),"
    "lightingGpiosPassword=document.getElementById('lighting-gpios-password'),"
    "saveLightingGpios=document.getElementById('save-lighting-gpios'),lightingGpiosStatus=document.getElementById('lighting-gpios-status'),"
    "lightingGpiosOptions=document.getElementById('lighting-gpios-options');let lightingGpiosLoaded=false,savedLightingGpios='';"
    "function validateLightingGpios(state){const valid=pins=>Array.isArray(pins)&&pins.length>0&&"
    "pins.every((pin,index)=>Number.isInteger(pin)&&pin>=0&&pin<=48&&(!index||pins[index-1]<pin));"
    "if(!state||!valid(state.gpios)||!valid(state.available_gpios)||"
    "state.gpios.some(pin=>!state.available_gpios.includes(pin)))throw new Error('Invalid GPIO response.');return state;}"
    "function validateSettings(settings){if(!settings||typeof settings.animation_enabled!=='boolean'||"
    "typeof settings.stick_figure_enabled!=='boolean')throw new Error('Invalid settings response.');return settings;}"
    "function restoreSettings(){animation.checked=savedSettings.animation_enabled;"
    "stickFigure.checked=savedSettings.stick_figure_enabled;}"
    "function updateSettingsControls(){const disabled=!settingsLoaded||settingsBusy||firmwareBusy;"
    "animation.disabled=disabled;stickFigure.disabled=disabled||!savedSettings.animation_enabled;"
    "lightingGpios.disabled=lightingGpiosPassword.disabled=saveLightingGpios.disabled="
    "firmwareBusy||settingsBusy||!enabled||!lightingGpiosLoaded;}"
    "async function loadSettings(){try{const response=await fetch('/settings',{cache:'no-store'});"
    "if(!response.ok)throw new Error('Could not load settings.');"
    "savedSettings=validateSettings(await response.json());settingsLoaded=true;restoreSettings();settingsStatus.textContent='';"
    "}catch(error){settingsStatus.textContent='Could not load settings. Reload this page to try again.';}"
    "finally{updateSettingsControls();updateAnimationTestControls();}}"
    "async function saveSetting(control){if(!settingsLoaded||settingsBusy||firmwareBusy||control.disabled){restoreSettings();return;}"
    "const requested=control.checked;settingsBusy=true;++animationStatusEpoch;updateSettingsControls();updateFirmwareControls();"
    "updateAnimationTestControls();settingsStatus.textContent='Saving…';"
    "try{const response=await fetch('/settings',{method:'POST',"
    "headers:{'Content-Type':'text/plain','X-Display-Settings':'1'},"
    "body:(control===stickFigure?'figure=':'')+(requested?'1':'0')});"
    "if(!response.ok){settingsStatus.textContent=await response.text()||'The device could not save this setting.';return;}"
    "savedSettings=validateSettings(await response.json());settingsStatus.textContent='Saved.';"
    "}catch(error){"
    "settingsStatus.textContent='Could not confirm the save. Reload this page to check the setting.';}"
    "finally{restoreSettings();settingsBusy=false;updateSettingsControls();updateFirmwareControls();"
    "updateAnimationTestControls();scheduleAnimationStatus(0);}}"
    "animation.addEventListener('change',()=>saveSetting(animation));"
    "stickFigure.addEventListener('change',()=>saveSetting(stickFigure));"
    "async function loadLightingGpios(){try{const response=await fetch('/lighting-gpios',{cache:'no-store'});"
    "if(!response.ok)throw new Error('Could not load GPIOs.');const state=validateLightingGpios(await response.json());"
    "savedLightingGpios=state.gpios.join(',');lightingGpios.value=savedLightingGpios;lightingGpiosLoaded=true;"
    "lightingGpiosOptions.textContent='Available GPIOs: '+state.available_gpios.join(', ')+'.';"
    "lightingGpiosStatus.textContent=enabled?'':'Password initialization failed. GPIO changes are disabled.';"
    "}catch(error){lightingGpiosStatus.textContent='Could not load GPIOs. Reload this page to try again.';}"
    "finally{updateSettingsControls();}}"
    "lightingGpiosForm.addEventListener('submit',async(event)=>{event.preventDefault();"
    "if(!enabled||!lightingGpiosLoaded||settingsBusy||firmwareBusy)return;"
    "if(!lightingGpiosPassword.value){lightingGpiosStatus.textContent='Enter the password.';return;}"
    "const requested=lightingGpios.value;"
    "settingsBusy=true;++animationStatusEpoch;updateSettingsControls();updateFirmwareControls();updateAnimationTestControls();"
    "lightingGpiosStatus.textContent='Saving…';try{const response=await fetch('/lighting-gpios',{method:'POST',"
    "headers:{'Content-Type':'text/plain','X-OTA-Password':lightingGpiosPassword.value},body:requested});"
    "if(!response.ok){lightingGpiosStatus.textContent=await response.text()||'The device could not save the GPIOs.';return;}"
    "const state=validateLightingGpios(await response.json());savedLightingGpios=state.gpios.join(',');"
    "lightingGpiosOptions.textContent='Available GPIOs: '+state.available_gpios.join(', ')+'.';lightingGpiosStatus.textContent='Saved.';"
    "}catch(error){lightingGpiosStatus.textContent='Could not confirm the save. Reload this page to check the GPIOs.';}"
    "finally{lightingGpios.value=savedLightingGpios;lightingGpiosPassword.value='';"
    "settingsBusy=false;updateSettingsControls();updateFirmwareControls();"
    "updateAnimationTestControls();scheduleAnimationStatus(0);}});"
    "const form=document.getElementById('upload'),"
    "button=document.getElementById('submit'),status=document.getElementById('status'),"
    "file=document.getElementById('firmware'),password=document.getElementById('password');"
    "const passwordForm=document.getElementById('change-password'),"
    "currentPassword=document.getElementById('current-password'),"
    "newPassword=document.getElementById('new-password'),"
    "confirmPassword=document.getElementById('confirm-password'),"
    "savePassword=document.getElementById('save-password'),"
    "passwordStatus=document.getElementById('password-status');let firmwareBusy=false,priceTestBusy=false,animationTestBusy=false;"
    "const testIncrease=document.getElementById('test-price-increase'),"
    "testDecrease=document.getElementById('test-price-decrease'),"
    "priceTestStatus=document.getElementById('price-test-status');"
    "const animationChoice=document.getElementById('animation-choice'),"
    "runAnimation=document.getElementById('run-animation'),"
    "animationTestStatus=document.getElementById('animation-test-status');let animationCatalogLoaded=false,animationCatalogSize=0;"
    "let animationStatusBusy=false,animationStatusEpoch=0,animationStatusTimer;"
    "const showPassword=document.getElementById('show-password'),"
    "showPasswordStatus=document.getElementById('show-password-status');"
    "const priceUrlForm=document.getElementById('price-url-form'),"
    "priceUrl=document.getElementById('price-url'),priceUrlPassword=document.getElementById('price-url-password'),"
    "savePriceUrl=document.getElementById('save-price-url'),priceUrlStatus=document.getElementById('price-url-status');"
    "let priceUrlLoaded=false;"
    "const enabled=form.dataset.enabled==='1';"
    "function updatePriceTestControls(){testIncrease.disabled=testDecrease.disabled=firmwareBusy||priceTestBusy;}"
    "function figureAnimationsEnabled(){return settingsLoaded&&savedSettings.animation_enabled&&savedSettings.stick_figure_enabled;}"
    "const enableSceneMessage='Enable Animation and Stick figure animation to run a scene.';"
    "function updateAnimationTestControls(){animationChoice.disabled=runAnimation.disabled="
    "firmwareBusy||settingsBusy||animationTestBusy||!animationCatalogLoaded||!figureAnimationsEnabled();"
    "if(animationCatalogLoaded){if(!settingsLoaded)animationTestStatus.textContent='Load display settings before running a scene.';"
    "else if(!figureAnimationsEnabled())animationTestStatus.textContent=enableSceneMessage;"
    "else if(animationTestStatus.textContent===enableSceneMessage||"
    "animationTestStatus.textContent==='Load display settings before running a scene.')animationTestStatus.textContent='';}}"
    "function updateFirmwareControls(){"
    "for(const control of [button,file,password,currentPassword,newPassword,confirmPassword,savePassword,showPassword])"
    "control.disabled=firmwareBusy||settingsBusy||!enabled;"
    "for(const control of [priceUrl,priceUrlPassword,savePriceUrl])"
    "control.disabled=firmwareBusy||settingsBusy||!enabled||!priceUrlLoaded;}"
    "function setFirmwareBusy(busy){firmwareBusy=busy;++animationStatusEpoch;updateFirmwareControls();updateSettingsControls();"
    "updatePriceTestControls();updateAnimationTestControls();}setFirmwareBusy(false);loadSettings();"
    "async function loadAnimations(){try{const response=await fetch('/api/animations',{cache:'no-store'});"
    "if(!response.ok)throw new Error('Could not load animations.');const catalog=await response.json();"
    "if(!Array.isArray(catalog)||!catalog.length||catalog.some((item,index)=>"
    "!item||item.id!==index+1||typeof item.name!=='string'||!item.name.trim()))throw new Error('Invalid animation catalog.');"
    "const options=catalog.map(item=>{const option=document.createElement('option');"
    "option.value=String(item.id);option.textContent=item.name;return option;});"
    "animationChoice.replaceChildren(...options);animationChoice.value=String(catalog[0].id);"
    "animationCatalogSize=catalog.length;animationCatalogLoaded=true;animationTestStatus.textContent='';"
    "}catch(error){animationTestStatus.textContent='Could not load animations. Reload this page to try again.';}"
    "finally{updateAnimationTestControls();}}loadAnimations();"
    "function scheduleAnimationStatus(delay=1000){clearTimeout(animationStatusTimer);"
    "animationStatusTimer=setTimeout(pollAnimationStatus,delay);}"
    "async function pollAnimationStatus(){if(animationStatusBusy)return;"
    "if(firmwareBusy||settingsBusy||animationTestBusy||!animationCatalogLoaded||!figureAnimationsEnabled()){"
    "scheduleAnimationStatus();return;}animationStatusBusy=true;const epoch=animationStatusEpoch;"
    "try{const response=await fetch('/api/animation',{cache:'no-store'});"
    "if(!response.ok)throw new Error('Could not check animation status.');const state=await response.json();"
    "if(!state||!Number.isInteger(state.animation_id)||state.animation_id<0||state.animation_id>animationCatalogSize||"
    "typeof state.running!=='boolean'||state.running!==(state.animation_id!==0)||typeof state.pending!=='boolean'||"
    "(state.running?(typeof state.name!=='string'||!state.name.trim()):state.name!==null))"
    "throw new Error('Invalid animation status.');"
    "if(epoch!==animationStatusEpoch||firmwareBusy||settingsBusy||animationTestBusy||!figureAnimationsEnabled())return;"
    "animationTestStatus.textContent=state.pending?'Starting animation…':"
    "state.running?'Playing: '+state.name+'.':'The figure is roaming.';"
    "}catch(error){if(epoch===animationStatusEpoch&&!firmwareBusy&&!settingsBusy&&!animationTestBusy&&figureAnimationsEnabled())"
    "animationTestStatus.textContent='Could not check animation status. Retrying…';}"
    "finally{animationStatusBusy=false;scheduleAnimationStatus();}}scheduleAnimationStatus();"
    "runAnimation.addEventListener('click',async()=>{if(firmwareBusy||settingsBusy||animationTestBusy||"
    "!animationCatalogLoaded||!figureAnimationsEnabled())return;"
    "animationTestBusy=true;++animationStatusEpoch;updateAnimationTestControls();animationTestStatus.textContent='Starting animation…';"
    "try{const response=await fetch('/api/animation',{method:'POST',"
    "headers:{'Content-Type':'text/plain','X-Animation-Test':'1'},body:animationChoice.value});"
    "const message=await response.text();if(!response.ok){"
    "animationTestStatus.textContent=message||'The display could not start the animation. Try again.';return;}"
    "if(!message.trim())throw new Error('Invalid animation response.');animationTestStatus.textContent='Starting: '+message+'.';"
    "}catch(error){animationTestStatus.textContent='Could not confirm the animation. Please try again.';}"
    "finally{animationTestBusy=false;updateAnimationTestControls();scheduleAnimationStatus(0);}});"
    "async function testPrice(change){if(firmwareBusy||priceTestBusy)return;"
    "priceTestBusy=true;updatePriceTestControls();priceTestStatus.textContent='Generating test price…';"
    "try{const response=await fetch('/test-price',{method:'POST',"
    "headers:{'Content-Type':'text/plain','X-Price-Test':'1'},body:change});"
    "const message=await response.text();if(!response.ok){"
    "priceTestStatus.textContent=message||'The display could not generate a test price. Try again.';return;}"
    "if(!message.trim())throw new Error('Invalid test-price response.');"
    "priceTestStatus.textContent='Test price: '+message+'. Live updates resume after 60 seconds.';"
    "}catch(error){priceTestStatus.textContent='Could not confirm the test price. Please try again.';}"
    "finally{priceTestBusy=false;updatePriceTestControls();}}"
    "testIncrease.addEventListener('click',()=>testPrice('+'));"
    "testDecrease.addEventListener('click',()=>testPrice('-'));"
    "async function loadPriceUrl(){try{const response=await fetch('/price-url',{cache:'no-store'});"
    "if(!response.ok)throw new Error('Could not load price URL.');priceUrl.value=await response.text();"
    "priceUrlLoaded=true;priceUrlStatus.textContent=enabled?'':'Password initialization failed. URL changes are disabled.';"
    "}catch(error){priceUrlStatus.textContent='Could not load the price URL. Reload this page to try again.';}"
    "finally{updateFirmwareControls();}}loadPriceUrl();loadLightingGpios();"
    "showPassword.addEventListener('click',async()=>{if(!enabled||firmwareBusy||settingsBusy)return;"
    "setFirmwareBusy(true);showPasswordStatus.textContent='Requesting…';"
    "try{const response=await fetch('/show-password',{method:'POST',headers:{'X-Display-Password':'1'}});"
    "showPasswordStatus.textContent=await response.text();"
    "}catch(error){showPasswordStatus.textContent='Could not confirm the request. Check the display or try again.';}"
    "finally{setFirmwareBusy(false);}});"
    "priceUrlForm.addEventListener('submit',async(event)=>{event.preventDefault();"
    "if(!enabled||firmwareBusy||settingsBusy||!priceUrlLoaded)return;const replacement=priceUrl.value;"
    "setFirmwareBusy(true);priceUrlStatus.textContent='Saving…';"
    "try{const response=await fetch('/price-url',{method:'POST',"
    "headers:{'Content-Type':'text/plain','X-OTA-Password':priceUrlPassword.value},body:replacement});"
    "const message=await response.text();priceUrlStatus.textContent=message;"
    "if(response.ok){priceUrlPassword.value='';priceUrlStatus.textContent='Saved.';}"
    "}catch(error){priceUrlStatus.textContent='Could not confirm the save. Reload this page to check the price URL.';}"
    "finally{setFirmwareBusy(false);}});"
    "if(!enabled){"
    "status.textContent='Password initialization failed. Uploads are disabled.';}"
    "passwordForm.addEventListener('submit',async(event)=>{event.preventDefault();"
    "if(!enabled||firmwareBusy||settingsBusy)return;const replacement=newPassword.value;"
    "if(replacement!==confirmPassword.value){passwordStatus.textContent='The new passwords do not match.';return;}"
    "if(!/^[\\x20-\\x7e]{1,128}$/.test(replacement)||replacement!==replacement.trim()){"
    "passwordStatus.textContent='Use 1 to 128 printable ASCII characters, without leading or trailing spaces.';return;}"
    "setFirmwareBusy(true);passwordStatus.textContent='Saving…';"
    "try{const response=await fetch('/password',{method:'POST',"
    "headers:{'Content-Type':'text/plain','X-OTA-Password':currentPassword.value},body:replacement});"
    "const message=await response.text();passwordStatus.textContent=message;"
    "if(response.ok){currentPassword.value='';newPassword.value='';confirmPassword.value='';"
    "password.value='';priceUrlPassword.value='';lightingGpiosPassword.value='';}"
    "}catch(error){passwordStatus.textContent='Could not confirm the change. Try the new password first on your next upload.';}"
    "finally{setFirmwareBusy(false);}});"
    "form.addEventListener('submit',async(event)=>{event.preventDefault();"
    "if(!enabled||firmwareBusy||settingsBusy)return;const firmware=file.files[0];"
    "if(!firmware||!firmware.size){status.textContent='Choose a non-empty .bin firmware file.';return;}"
    "if(!firmware.name.toLowerCase().endsWith('.bin')){status.textContent='Choose a .bin firmware file.';return;}"
    "setFirmwareBusy(true);"
    "status.textContent='Uploading and validating firmware. Keep the display powered.';"
    "try{const response=await fetch('/update',{method:'POST',"
    "headers:{'Content-Type':'application/octet-stream','X-OTA-Password':password.value},"
    "body:firmware});const message=await response.text();status.textContent=message;"
    "if(response.ok){password.value='';button.textContent='Upgrade complete';return;}"
    "}catch(error){status.textContent='The connection was interrupted. Wait for the display, then reload this page to check or retry.';}"
    "setFirmwareBusy(false);"
    "});</script></html>";

static void set_common_headers(httpd_req_t *request)
{
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    httpd_resp_set_hdr(request, "X-Content-Type-Options", "nosniff");
    httpd_resp_set_hdr(request, "Connection", "close");
}

static esp_err_t reply(httpd_req_t *request, const char *status, const char *message)
{
    set_common_headers(request);
    httpd_resp_set_status(request, status);
    httpd_resp_set_type(request, "text/plain; charset=utf-8");
    return httpd_resp_send(request, message, HTTPD_RESP_USE_STRLEN);
}

/* Close rejected uploads immediately, rather than consuming their remaining body. */
static esp_err_t reject(httpd_req_t *request, const char *status, const char *message)
{
    reply(request, status, message);
    return ESP_FAIL;
}

static esp_err_t page_handler(httpd_req_t *request)
{
    set_common_headers(request);
    httpd_resp_set_type(request, "text/html; charset=utf-8");
    httpd_resp_set_hdr(request, "Content-Security-Policy",
                       "default-src 'none'; style-src 'unsafe-inline'; "
                       "script-src 'unsafe-inline'; connect-src 'self'; "
                       "form-action 'none'; base-uri 'none'; frame-ancestors 'none'");
    esp_err_t result = httpd_resp_send_chunk(request, PAGE_START, sizeof(PAGE_START) - 1);
    if (result == ESP_OK) {
        result = httpd_resp_send_chunk(request, s_password_length ? "1" : "0", 1);
    }
    if (result == ESP_OK) {
        result = httpd_resp_send_chunk(request, PAGE_END, sizeof(PAGE_END) - 1);
    }
    if (result == ESP_OK) {
        result = httpd_resp_send_chunk(request, NULL, 0);
    }
    return result;
}

static esp_err_t settings_reply(httpd_req_t *request)
{
    set_common_headers(request);
    httpd_resp_set_status(request, "200 OK");
    httpd_resp_set_type(request, "application/json");
    const display_settings_state_t settings = display_settings_get();
    char body[96];
    snprintf(body, sizeof(body), "{\"animation_enabled\":%s,\"stick_figure_enabled\":%s}",
             settings.animation_enabled ? "true" : "false",
             settings.stick_figure_enabled ? "true" : "false");
    return httpd_resp_send(request, body, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t settings_get_handler(httpd_req_t *request)
{
    return settings_reply(request);
}

static esp_err_t settings_post_handler(httpd_req_t *request)
{
    /* A cross-site HTML form cannot send this header; CORS is not enabled. */
    char settings_header[2];
    if (httpd_req_get_hdr_value_len(request, "X-Display-Settings") != 1 ||
        httpd_req_get_hdr_value_str(request, "X-Display-Settings", settings_header,
                                   sizeof(settings_header)) != ESP_OK ||
        settings_header[0] != '1') {
        return reject(request, "403 Forbidden", "Save settings from the display controls page.");
    }
    if (s_restart_pending) {
        return reject(request, "503 Service Unavailable", "The display is restarting. Wait, then reload the page.");
    }
    if (request->content_len != 1 && request->content_len != 8) {
        return reject(request, "400 Bad Request", "Send exactly 1, 0, figure=1, or figure=0.");
    }

    char content_type[64];
    const size_t type_length = httpd_req_get_hdr_value_len(request, "Content-Type");
    if (!type_length || type_length >= sizeof(content_type) ||
        httpd_req_get_hdr_value_str(request, "Content-Type", content_type,
                                   sizeof(content_type)) != ESP_OK ||
        strncasecmp(content_type, "text/plain", 10) != 0 ||
        (content_type[10] != '\0' && content_type[10] != ';')) {
        return reject(request, "415 Unsupported Media Type", "Send the animation setting as plain text.");
    }

    char value[8];
    size_t received_total = 0;
    while (received_total < request->content_len) {
        const int received = httpd_req_recv(request, value + received_total,
                                           request->content_len - received_total);
        if (received <= 0) {
            if (received == HTTPD_SOCK_ERR_TIMEOUT) {
                return reject(request, "408 Request Timeout", "Saving the setting timed out. Please try again.");
            }
            return ESP_FAIL;
        }
        received_total += (size_t)received;
    }
    const bool figure_setting = received_total == 8;
    const char enabled = value[received_total - 1];
    if ((figure_setting && memcmp(value, "figure=", 7) != 0) ||
        (enabled != '0' && enabled != '1')) {
        return reject(request, "400 Bad Request", "Send exactly 1, 0, figure=1, or figure=0.");
    }

    const esp_err_t result = figure_setting
        ? display_settings_set_stick_figure_enabled(enabled == '1')
        : display_settings_set_animation_enabled(enabled == '1');
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "Cannot save display settings: %s", esp_err_to_name(result));
        return reject(request, "500 Internal Server Error", "The device could not save this setting. Please try again.");
    }
    return settings_reply(request);
}

static esp_err_t lighting_gpios_get_handler(httpd_req_t *request)
{
    if (s_restart_pending) {
        return reject(request, "503 Service Unavailable", "The display is restarting. Wait, then reload the page.");
    }
    char selected[LIGHTING_GPIO_TEXT_CAPACITY];
    char available[LIGHTING_GPIO_TEXT_CAPACITY];
    if (lighting_gpio_format(lighting_settings_get(), selected, sizeof(selected)) != ESP_OK ||
        lighting_gpio_format(lighting_gpio_available_mask(), available, sizeof(available)) != ESP_OK) {
        return reject(request, "500 Internal Server Error", "The device could not read the GPIO settings. Please try again.");
    }
    char body[2 * LIGHTING_GPIO_TEXT_CAPACITY + 64];
    const int length = snprintf(body, sizeof(body), "{\"gpios\":[%s],\"available_gpios\":[%s]}", selected, available);
    if (length < 0 || (size_t)length >= sizeof(body)) return ESP_FAIL;
    set_common_headers(request);
    httpd_resp_set_status(request, "200 OK");
    httpd_resp_set_type(request, "application/json");
    return httpd_resp_send(request, body, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t lighting_gpios_post_handler(httpd_req_t *request)
{
    if (!s_password_length) {
        return reject(request, "403 Forbidden", "Password initialization failed. Restart the display and try again.");
    }
    /* Authentication also supplies the custom header required to block
     * cross-site HTML form submissions; CORS is not enabled. */
    if (!authorized(request)) {
        return reject(request, "401 Unauthorized", "The password is incorrect.");
    }
    if (s_restart_pending) {
        return reject(request, "503 Service Unavailable", "The display is restarting. Wait, then reload the page.");
    }
    if (!request->content_len) {
        return reject(request, "400 Bad Request", "Choose at least one available GPIO.");
    }
    if (request->content_len >= LIGHTING_GPIO_TEXT_CAPACITY) {
        return reject(request, "413 Content Too Large", "The GPIO list is too long.");
    }
    char content_type[64];
    const size_t type_length = httpd_req_get_hdr_value_len(request, "Content-Type");
    if (!type_length || type_length >= sizeof(content_type) ||
        httpd_req_get_hdr_value_str(request, "Content-Type", content_type, sizeof(content_type)) != ESP_OK ||
        strncasecmp(content_type, "text/plain", 10) != 0 ||
        (content_type[10] != '\0' && content_type[10] != ';')) {
        return reject(request, "415 Unsupported Media Type", "Send the GPIO list as plain text.");
    }
    char value[LIGHTING_GPIO_TEXT_CAPACITY];
    size_t received_total = 0;
    while (received_total < request->content_len) {
        const int received = httpd_req_recv(request, value + received_total, request->content_len - received_total);
        if (received <= 0) {
            if (received == HTTPD_SOCK_ERR_TIMEOUT) {
                return reject(request, "408 Request Timeout", "Saving GPIOs timed out. Please try again.");
            }
            return ESP_FAIL;
        }
        received_total += (size_t)received;
    }
    if (memchr(value, '\0', received_total)) {
        return reject(request, "400 Bad Request", "Use distinct available GPIO numbers separated by commas.");
    }
    value[received_total] = '\0';
    uint64_t mask;
    if (lighting_gpio_parse(value, &mask) != ESP_OK) {
        return reject(request, "400 Bad Request", "Use distinct available GPIO numbers separated by commas.");
    }
    const esp_err_t result = lighting_settings_set(mask);
    if (result == ESP_ERR_INVALID_ARG) {
        return reject(request, "400 Bad Request", "Use distinct available GPIO numbers separated by commas.");
    }
    if (result == ESP_ERR_INVALID_STATE || result == ESP_ERR_TIMEOUT) {
        return reject(request, "503 Service Unavailable", "The display is busy. Please try the GPIO change again.");
    }
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "Cannot save lighting GPIOs: %s", esp_err_to_name(result));
        return reject(request, "500 Internal Server Error", "The device could not save the GPIOs. Please try again.");
    }
    return lighting_gpios_get_handler(request);
}

static esp_err_t price_test_post_handler(httpd_req_t *request)
{
    /* Same-origin custom header; cross-site HTML forms cannot supply it. */
    char test_header[2];
    if (httpd_req_get_hdr_value_len(request, "X-Price-Test") != 1 ||
        httpd_req_get_hdr_value_str(request, "X-Price-Test", test_header,
                                   sizeof(test_header)) != ESP_OK ||
        test_header[0] != '1') {
        return reject(request, "403 Forbidden", "Test prices from the display controls page.");
    }
    if (s_restart_pending) {
        return reject(request, "503 Service Unavailable", "The display is restarting. Wait, then reload the page.");
    }
    if (request->content_len != 1) {
        return reject(request, "400 Bad Request", "The price-test direction must be exactly + or -.");
    }
    char content_type[64];
    const size_t type_length = httpd_req_get_hdr_value_len(request, "Content-Type");
    if (!type_length || type_length >= sizeof(content_type) ||
        httpd_req_get_hdr_value_str(request, "Content-Type", content_type,
                                   sizeof(content_type)) != ESP_OK ||
        strncasecmp(content_type, "text/plain", 10) != 0 ||
        (content_type[10] != '\0' && content_type[10] != ';')) {
        return reject(request, "415 Unsupported Media Type", "Send the price-test direction as plain text.");
    }
    char value;
    const int received = httpd_req_recv(request, &value, sizeof(value));
    if (received <= 0) {
        if (received == HTTPD_SOCK_ERR_TIMEOUT) {
            return reject(request, "408 Request Timeout", "Testing the price timed out. Please try again.");
        }
        return ESP_FAIL;
    }
    if (value != '+' && value != '-') {
        return reject(request, "400 Bad Request", "The price-test direction must be exactly + or -.");
    }
    char price[64] = {0};
    const esp_err_t result = price_test_request(value == '+', price, sizeof(price));
    if (result == ESP_ERR_INVALID_STATE || result == ESP_ERR_NO_MEM) {
        return reject(request, "503 Service Unavailable", "The display is busy. Please try the price test again.");
    }
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "Cannot generate a test price: %s", esp_err_to_name(result));
        return reject(request, "500 Internal Server Error", "The display could not generate a test price. Please try again.");
    }
    return reply(request, "200 OK", price);
}

static esp_err_t animations_get_handler(httpd_req_t *request)
{
    set_common_headers(request);
    httpd_resp_set_status(request, "200 OK");
    httpd_resp_set_type(request, "application/json");
    esp_err_t result = httpd_resp_send_chunk(request, "[", 1);
    for (size_t i = 0; result == ESP_OK && i < ANIMATION_COUNT; ++i) {
        const animation_info_t *info = &animation_catalog[i];
        char item[192];
        /* Names in the static catalog contain no JSON escape characters. */
        const int length = snprintf(item, sizeof(item),
            "%s{\"id\":%u,\"name\":\"%s\",\"allowed_states\":%u,\"duration_ms\":%lu}",
            i ? "," : "", (unsigned)info->id, info->name,
            (unsigned)info->allowed_states, (unsigned long)info->duration_ms);
        if (length < 0 || (size_t)length >= sizeof(item)) return ESP_FAIL;
        result = httpd_resp_send_chunk(request, item, (size_t)length);
    }
    if (result == ESP_OK) result = httpd_resp_send_chunk(request, "]", 1);
    if (result == ESP_OK) result = httpd_resp_send_chunk(request, NULL, 0);
    return result;
}

static esp_err_t animation_post_handler(httpd_req_t *request)
{
    /* CORS stays disabled; cross-site forms cannot supply this custom header. */
    char test_header[2];
    if (httpd_req_get_hdr_value_len(request, "X-Animation-Test") != 1 ||
        httpd_req_get_hdr_value_str(request, "X-Animation-Test", test_header,
                                   sizeof(test_header)) != ESP_OK ||
        test_header[0] != '1') {
        return reject(request, "403 Forbidden", "Run animations from the display controls page.");
    }
    if (s_restart_pending) {
        return reject(request, "503 Service Unavailable", "The display is restarting. Wait, then reload the page.");
    }
    if (!request->content_len || request->content_len > 2) {
        return reject(request, "400 Bad Request", "Choose an animation from the list.");
    }
    char content_type[64];
    const size_t type_length = httpd_req_get_hdr_value_len(request, "Content-Type");
    if (!type_length || type_length >= sizeof(content_type) ||
        httpd_req_get_hdr_value_str(request, "Content-Type", content_type,
                                   sizeof(content_type)) != ESP_OK ||
        strncasecmp(content_type, "text/plain", 10) != 0 ||
        (content_type[10] != '\0' && content_type[10] != ';')) {
        return reject(request, "415 Unsupported Media Type", "Send the animation ID as plain text.");
    }
    char value[2];
    size_t received_total = 0;
    while (received_total < request->content_len) {
        const int received = httpd_req_recv(request, value + received_total,
                                           request->content_len - received_total);
        if (received <= 0) {
            if (received == HTTPD_SOCK_ERR_TIMEOUT) {
                return reject(request, "408 Request Timeout", "Starting the animation timed out. Please try again.");
            }
            return ESP_FAIL;
        }
        received_total += (size_t)received;
    }
    unsigned number = 0;
    for (size_t i = 0; i < received_total; ++i) {
        if (value[i] < '0' || value[i] > '9' || (i == 0 && value[i] == '0')) {
            return reject(request, "400 Bad Request", "Choose an animation from the list.");
        }
        number = number * 10 + (unsigned)(value[i] - '0');
    }
    const animation_info_t *info = animation_catalog_lookup((animation_id_t)number);
    if (!info) return reject(request, "400 Bad Request", "Choose an animation from the list.");
    const display_settings_state_t settings = display_settings_get();
    if (!settings.animation_enabled || !settings.stick_figure_enabled) {
        return reject(request, "409 Conflict", "Enable Animation and Stick figure animation to run a scene.");
    }
    const esp_err_t result = animation_test_request(info->id);
    if (result == ESP_ERR_INVALID_STATE || result == ESP_ERR_NO_MEM || result == ESP_ERR_TIMEOUT) {
        return reject(request, "503 Service Unavailable", "The display is busy. Please try the animation again.");
    }
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "Cannot queue animation: %s", esp_err_to_name(result));
        return reject(request, "500 Internal Server Error", "The display could not start the animation. Please try again.");
    }
    return reply(request, "200 OK", info->name);
}

static esp_err_t animation_get_handler(httpd_req_t *request)
{
    if (s_restart_pending) {
        return reject(request, "503 Service Unavailable", "The display is restarting. Wait, then reload the page.");
    }
    const display_settings_state_t settings = display_settings_get();
    animation_status_t status = animation_test_status();
    if (!settings.animation_enabled || !settings.stick_figure_enabled) {
        status.animation_id = ANIMATION_NONE;
        status.pending = false;
    }
    const animation_info_t *info = animation_catalog_lookup(status.animation_id);
    char name[160];
    if (info) snprintf(name, sizeof(name), "\"%s\"", info->name);
    else strcpy(name, "null");
    char body[256];
    snprintf(body, sizeof(body),
             "{\"animation_id\":%u,\"running\":%s,\"pending\":%s,\"name\":%s}",
             info ? (unsigned)info->id : 0, info ? "true" : "false",
             status.pending ? "true" : "false", name);
    set_common_headers(request);
    httpd_resp_set_status(request, "200 OK");
    httpd_resp_set_type(request, "application/json");
    return httpd_resp_send(request, body, HTTPD_RESP_USE_STRLEN);
}

static void clear_password(char *password, size_t size)
{
    volatile char *bytes = password;
    while (size--) *bytes++ = 0;
}

static bool authorized(httpd_req_t *request)
{
    char provided[OTA_PASSWORD_MAX_LENGTH + 1] = {0};
    const size_t length = httpd_req_get_hdr_value_len(request, "X-OTA-Password");
    if (!s_password_length || !length || length > OTA_PASSWORD_MAX_LENGTH ||
        httpd_req_get_hdr_value_str(request, "X-OTA-Password", provided,
                                   sizeof(provided)) != ESP_OK) {
        return false;
    }

    /* Compare a fixed number of bytes, including padding, without an early exit. */
    volatile unsigned difference = (unsigned)(length != s_password_length);
    for (size_t i = 0; i < OTA_PASSWORD_MAX_LENGTH; ++i) {
        difference |= (unsigned char)provided[i] ^ (unsigned char)s_password[i];
    }
    clear_password(provided, sizeof(provided));
    return difference == 0;
}

static esp_err_t show_password_post_handler(httpd_req_t *request)
{
    /* A custom header prevents cross-origin forms from revealing the password. */
    char display_header[2] = {0};
    if (httpd_req_get_hdr_value_len(request, "X-Display-Password") != 1 ||
        httpd_req_get_hdr_value_str(request, "X-Display-Password", display_header,
                                   sizeof(display_header)) != ESP_OK ||
        display_header[0] != '1') {
        return reject(request, "400 Bad Request", "Use the display controls page to show the password.");
    }
    if (request->content_len != 0) {
        return reject(request, "400 Bad Request", "The display request must have an empty body.");
    }
    if (!s_password_length) {
        return reject(request, "403 Forbidden", "Password initialization failed. Restart the display and try again.");
    }
    if (s_restart_pending) {
        return reject(request, "503 Service Unavailable", "The display is restarting. Wait, then reload the page.");
    }
    /* The display module copies the secret into its private request buffer. */
    const esp_err_t result = password_display_request(s_password);
    if (result == ESP_ERR_INVALID_STATE) {
        return reject(request, "503 Service Unavailable", "The display is busy. Please try again shortly.");
    }
    if (result != ESP_OK) {
        return reject(request, "500 Internal Server Error", "The device could not show the password. Please try again.");
    }
    return reply(request, "200 OK", "Password is showing on the display.");
}

static esp_err_t price_url_get_handler(httpd_req_t *request)
{
    char url[SERVICE_URL_MAX_LENGTH + 1];
    const esp_err_t result = service_get_url(url, sizeof(url));
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "Cannot load price URL: %s", esp_err_to_name(result));
        return reject(request, "500 Internal Server Error", "The device could not load the price URL. Please try again.");
    }
    return reply(request, "200 OK", url);
}

static esp_err_t price_url_post_handler(httpd_req_t *request)
{
    if (!s_password_length) {
        return reject(request, "403 Forbidden", "Password initialization failed. Restart the display and try again.");
    }
    if (!authorized(request)) {
        return reject(request, "401 Unauthorized", "The password is incorrect.");
    }
    if (s_restart_pending) {
        return reject(request, "503 Service Unavailable", "The display is restarting. Wait, then reload the page.");
    }
    if (request->content_len > SERVICE_URL_MAX_LENGTH) {
        return reject(request, "413 Payload Too Large", "The price URL must be at most 1024 characters.");
    }
    char content_type[64];
    const size_t type_length = httpd_req_get_hdr_value_len(request, "Content-Type");
    if (!type_length || type_length >= sizeof(content_type) ||
        httpd_req_get_hdr_value_str(request, "Content-Type", content_type,
                                   sizeof(content_type)) != ESP_OK ||
        strncasecmp(content_type, "text/plain", 10) != 0 ||
        (content_type[10] != '\0' && content_type[10] != ';')) {
        return reject(request, "415 Unsupported Media Type", "Send the price URL as plain text.");
    }

    char url[SERVICE_URL_MAX_LENGTH + 1] = {0};
    size_t received_total = 0;
    while (received_total < request->content_len) {
        const int received = httpd_req_recv(request, url + received_total,
                                           request->content_len - received_total);
        if (received <= 0) {
            if (received == HTTPD_SOCK_ERR_TIMEOUT) {
                return reject(request, "408 Request Timeout", "Saving the price URL timed out. Please try again.");
            }
            return reject(request, "400 Bad Request", "The price URL was not received completely.");
        }
        if (memchr(url + received_total, '\0', (size_t)received)) {
            return reject(request, "400 Bad Request", "Use an HTTP or HTTPS URL, or leave it empty to pause price updates.");
        }
        received_total += (size_t)received;
    }

    const esp_err_t result = service_set_url(url);
    if (result == ESP_ERR_INVALID_ARG) {
        return reject(request, "400 Bad Request", "Use an HTTP or HTTPS URL, or leave it empty to pause price updates.");
    }
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "Cannot save price URL: %s", esp_err_to_name(result));
        return reject(request, "500 Internal Server Error", "The device could not save the price URL. Please try again.");
    }
    return reply(request, "200 OK", url[0] ? "Price URL saved." : "Price fetching disabled.");
}

static esp_err_t password_post_handler(httpd_req_t *request)
{
    if (!s_password_length) {
        return reject(request, "403 Forbidden", "Password initialization failed. Restart the display and try again.");
    }
    /* The custom authentication header also prevents cross-site HTML form posts. */
    if (!authorized(request)) {
        return reject(request, "401 Unauthorized", "The current password is incorrect.");
    }
    if (s_restart_pending) {
        return reject(request, "503 Service Unavailable", "The display is restarting. Wait, then reload the page.");
    }
    if (!request->content_len || request->content_len > OTA_PASSWORD_MAX_LENGTH) {
        return reject(request, "400 Bad Request", "Use 1 to 128 printable ASCII characters, without leading or trailing spaces.");
    }
    char content_type[64];
    const size_t type_length = httpd_req_get_hdr_value_len(request, "Content-Type");
    if (!type_length || type_length >= sizeof(content_type) ||
        httpd_req_get_hdr_value_str(request, "Content-Type", content_type,
                                   sizeof(content_type)) != ESP_OK ||
        strncasecmp(content_type, "text/plain", 10) != 0 ||
        (content_type[10] != '\0' && content_type[10] != ';')) {
        return reject(request, "415 Unsupported Media Type", "Send the new password as plain text.");
    }

    char replacement[OTA_PASSWORD_MAX_LENGTH + 1] = {0};
    size_t received_total = 0;
    while (received_total < request->content_len) {
        const int received = httpd_req_recv(request, replacement + received_total,
                                           request->content_len - received_total);
        if (received <= 0) {
            clear_password(replacement, sizeof(replacement));
            if (received == HTTPD_SOCK_ERR_TIMEOUT) {
                return reject(request, "408 Request Timeout", "Changing the password timed out. Please try again.");
            }
            return ESP_FAIL;
        }
        for (int i = 0; i < received; ++i) {
            const unsigned char value = (unsigned char)replacement[received_total + (size_t)i];
            if (value < 32 || value > 126) {
                clear_password(replacement, sizeof(replacement));
                return reject(request, "400 Bad Request", "Use 1 to 128 printable ASCII characters, without leading or trailing spaces.");
            }
        }
        received_total += (size_t)received;
    }

    if (replacement[0] == ' ' || replacement[received_total - 1] == ' ') {
        clear_password(replacement, sizeof(replacement));
        return reject(request, "400 Bad Request", "Use 1 to 128 printable ASCII characters, without leading or trailing spaces.");
    }
    const esp_err_t result = upgrade_password_save(replacement);
    if (result == ESP_OK) {
        /* Keep the active credential unchanged until the flash commit succeeds. */
        clear_password(s_password, sizeof(s_password));
        memcpy(s_password, replacement, received_total);
        s_password_length = received_total;
    }
    clear_password(replacement, sizeof(replacement));
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "Cannot save password: %s", esp_err_to_name(result));
        return reject(request, "500 Internal Server Error", "The device could not save the new password. Please try again.");
    }
    return reply(request, "200 OK", "Password changed.");
}

static void reboot_task(void *argument)
{
    (void)argument;
    /* The handler notifies this task after sending the final HTTP response. */
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    vTaskDelay(pdMS_TO_TICKS(OTA_REBOOT_DELAY_MS));
    esp_restart();
}

static esp_err_t update_handler(httpd_req_t *request)
{
    if (!s_password_length) {
        return reject(request, "403 Forbidden",
                      "Password initialization failed. Uploads are disabled.");
    }
    if (!authorized(request)) {
        return reject(request, "401 Unauthorized", "The password is incorrect.");
    }
    if (s_restart_pending) {
        return reject(request, "503 Service Unavailable", "An upgrade is complete; the display is restarting.");
    }

    const esp_partition_t *partition = esp_ota_get_next_update_partition(NULL);
    if (!partition) {
        return reject(request, "500 Internal Server Error", "No inactive OTA application partition is available.");
    }
    if (request->content_len < sizeof(esp_image_header_t)) {
        return reject(request, "400 Bad Request", "Choose a non-empty application firmware .bin file.");
    }
    if (request->content_len > partition->size) {
        return reject(request, "413 Content Too Large", "This firmware is larger than the available OTA partition.");
    }

    const esp_err_t identity_result = matter_display_prepare_ota();
    if (identity_result != ESP_OK) {
        ESP_LOGE(TAG, "Cannot protect the original Matter identity for OTA: %s", esp_err_to_name(identity_result));
        return reject(request, "503 Service Unavailable",
                      "The original Matter pairing identity could not be verified. Firmware was not changed.");
    }

    esp_ota_handle_t handle;
    esp_err_t result = esp_ota_begin(partition, request->content_len, &handle);
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "Cannot begin OTA: %s", esp_err_to_name(result));
        return reject(request, "500 Internal Server Error", "The device could not prepare the firmware upgrade.");
    }

    char buffer[OTA_RECEIVE_BUFFER_SIZE];
    size_t remaining = request->content_len;
    while (remaining) {
        const size_t wanted = remaining < sizeof(buffer) ? remaining : sizeof(buffer);
        const int received = httpd_req_recv(request, buffer, wanted);
        if (received <= 0) {
            esp_ota_abort(handle);
            if (received == HTTPD_SOCK_ERR_TIMEOUT) {
                return reject(request, "408 Request Timeout", "The upload timed out. The current firmware is unchanged; retry the upload.");
            }
            ESP_LOGW(TAG, "Firmware upload interrupted");
            /* A disconnected socket cannot reliably receive a response. */
            return ESP_FAIL;
        }
        result = esp_ota_write(handle, buffer, (size_t)received);
        if (result != ESP_OK) {
            esp_ota_abort(handle);
            ESP_LOGE(TAG, "Cannot write OTA image: %s", esp_err_to_name(result));
            if (result == ESP_ERR_OTA_VALIDATE_FAILED) {
                return reject(request, "400 Bad Request", "This is not a valid application firmware image.");
            }
            return reject(request, "500 Internal Server Error", "The device could not write the firmware upgrade. The current firmware is unchanged.");
        }
        remaining -= (size_t)received;
    }

    /* esp_ota_end frees the handle on success and failure; do not abort it again. */
    result = esp_ota_end(handle);
    if (result != ESP_OK) {
        ESP_LOGW(TAG, "OTA image validation failed: %s", esp_err_to_name(result));
        return reject(request, "400 Bad Request", "Firmware validation failed. Upload the application .bin for this ESP32-S3 display.");
    }

    /* Allocate before selecting the image, so low memory cannot strand a pending upgrade. */
    TaskHandle_t restart_task = NULL;
    if (xTaskCreate(reboot_task, "ota_reboot", 2048, NULL, 5, &restart_task) != pdPASS) {
        return reject(request, "500 Internal Server Error", "The device could not schedule a restart. Retry the upload.");
    }
    result = esp_ota_set_boot_partition(partition);
    if (result != ESP_OK) {
        vTaskDelete(restart_task);
        ESP_LOGE(TAG, "Cannot select OTA boot partition: %s", esp_err_to_name(result));
        return reject(request, "500 Internal Server Error", "The device could not activate the new firmware. The current firmware is unchanged.");
    }

    s_restart_pending = true;
    ESP_LOGI(TAG, "Firmware upgrade complete (%zu bytes); restarting", request->content_len);
    result = reply(request, "200 OK", "Firmware verified. The display is restarting; wait a few seconds, then reload this page.");
    /* Even if the client disconnects now, the validated image has been selected. */
    xTaskNotifyGive(restart_task);
    return result;
}

esp_err_t ota_server_start(void)
{
    if (s_server) {
        return ESP_OK;
    }

    clear_password(s_password, sizeof(s_password));
    s_password_length = 0;
    esp_err_t result = upgrade_password_load(s_password, sizeof(s_password));
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "Password initialization failed: %s", esp_err_to_name(result));
        return result;
    }
    s_password_length = strlen(s_password);
    if (!s_password_length) {
        ESP_LOGE(TAG, "Password initialization failed: empty password; protected changes disabled");
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.stack_size = 8192;
    config.recv_wait_timeout = 15;
    config.send_wait_timeout = 10;
    config.lru_purge_enable = true;
    config.max_uri_handlers = 14;

    result = httpd_start(&s_server, &config);
    if (result != ESP_OK) {
        s_server = NULL;
        return result;
    }
    const httpd_uri_t page = {.uri = "/", .method = HTTP_GET, .handler = page_handler};
    const httpd_uri_t update = {.uri = "/update", .method = HTTP_POST, .handler = update_handler};
    const httpd_uri_t settings_get = {.uri = "/settings", .method = HTTP_GET, .handler = settings_get_handler};
    const httpd_uri_t settings_post = {.uri = "/settings", .method = HTTP_POST, .handler = settings_post_handler};
    const httpd_uri_t password_post = {.uri = "/password", .method = HTTP_POST, .handler = password_post_handler};
    const httpd_uri_t show_password_post = {.uri = "/show-password", .method = HTTP_POST, .handler = show_password_post_handler};
    const httpd_uri_t price_url_get = {.uri = "/price-url", .method = HTTP_GET, .handler = price_url_get_handler};
    const httpd_uri_t price_url_post = {.uri = "/price-url", .method = HTTP_POST, .handler = price_url_post_handler};
    const httpd_uri_t price_test_post = {.uri = "/test-price", .method = HTTP_POST, .handler = price_test_post_handler};
    const httpd_uri_t animations_get = {.uri = "/api/animations", .method = HTTP_GET, .handler = animations_get_handler};
    const httpd_uri_t animation_post = {.uri = "/api/animation", .method = HTTP_POST, .handler = animation_post_handler};
    const httpd_uri_t animation_get = {.uri = "/api/animation", .method = HTTP_GET, .handler = animation_get_handler};
    const httpd_uri_t lighting_gpios_get = {.uri = "/lighting-gpios", .method = HTTP_GET, .handler = lighting_gpios_get_handler};
    const httpd_uri_t lighting_gpios_post = {.uri = "/lighting-gpios", .method = HTTP_POST, .handler = lighting_gpios_post_handler};
    result = httpd_register_uri_handler(s_server, &page);
    if (result == ESP_OK) {
        result = httpd_register_uri_handler(s_server, &update);
    }
    if (result == ESP_OK) {
        result = httpd_register_uri_handler(s_server, &settings_get);
    }
    if (result == ESP_OK) {
        result = httpd_register_uri_handler(s_server, &settings_post);
    }
    if (result == ESP_OK) {
        result = httpd_register_uri_handler(s_server, &password_post);
    }
    if (result == ESP_OK) {
        result = httpd_register_uri_handler(s_server, &show_password_post);
    }
    if (result == ESP_OK) {
        result = httpd_register_uri_handler(s_server, &price_url_get);
    }
    if (result == ESP_OK) {
        result = httpd_register_uri_handler(s_server, &price_url_post);
    }
    if (result == ESP_OK) {
        result = httpd_register_uri_handler(s_server, &price_test_post);
    }
    if (result == ESP_OK) {
        result = httpd_register_uri_handler(s_server, &animations_get);
    }
    if (result == ESP_OK) {
        result = httpd_register_uri_handler(s_server, &animation_post);
    }
    if (result == ESP_OK) {
        result = httpd_register_uri_handler(s_server, &animation_get);
    }
    if (result == ESP_OK) {
        result = httpd_register_uri_handler(s_server, &lighting_gpios_get);
    }
    if (result == ESP_OK) {
        result = httpd_register_uri_handler(s_server, &lighting_gpios_post);
    }
    if (result != ESP_OK) {
        httpd_stop(s_server);
        s_server = NULL;
        return result;
    }
    ESP_LOGI(TAG, "Display controls page available on HTTP port %u", config.server_port);
    return ESP_OK;
}
