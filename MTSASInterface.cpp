/* MTSAS implementation of NetworkInterfaceAPI
 * Copyright (c) 2015 ARM Limited
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
 
#include "MTSASInterface.h"
#include <string> 
/** MTSASInterface class
 *  Implementation of the NetworkInterface for MTS DRAGONFLY
 */

enum registration_status {  NOT_REGISTERED = 0, 
                            REGISTERED = 1, 
                            SEARCHING = 2, 
                            DENIED = 3, 
                            UNKNOWN = 4, 
                            ROAMING = 5};

#define MTSAS_MISC_TIMEOUT 3000
#define MTSAS_RESTART_TIMEOUT 10000
#define MTSAS_COMMUNICATION_TIMEOUT 100

MTSASInterface::MTSASInterface(PinName tx, PinName rx, bool debug)
    : _serial(tx, rx, 1024), _parser(_serial), reset(PinName(RESET))
{
    _parser.debugOn(debug);
    // Register message indicating incoming data as out of band 
    // data (data that can come at any time)
    _parser.oob("SRING:",callback(this, &MTSASInterface::event));
    _parser.setTimeout(MTSAS_MISC_TIMEOUT);
    _debug = debug;
    _serial.baud(115200);
    // Serial RX will signal the event thread
    _serial.attach(callback(this, &MTSASInterface::rx_sem_release));
    event_thread.start(callback(this, &MTSASInterface::handle_event));
    memset(_socket_ids, 0 , sizeof(_socket_ids));
    memset(_cbs, 0, sizeof(_cbs));
    //PDP context
    context = 1;
}

MTSASInterface::~MTSASInterface(){
}

nsapi_error_t MTSASInterface::set_credentials(const char *apn,
    const char *username , const char *password)
{
    at_mutex.lock();
    for (int i=1; i < MTSAS_SOCKET_COUNT; i++){
        //Socket configuration 
        //AT#SCFG=<socket id>,<PDP context>,<packet size default 300>,
        //        <exchange timeout>,<connection to>,<txto>
        _parser.send("AT#SCFG=%d,%d,0,0,600,0",i,context);
        _parser.recv("OK");
    }
    //Activate the PDP context 
    int ret = NSAPI_ERROR_DEVICE_ERROR;
    if (_parser.send("AT+CGDCONT=%d,\"IP\",\"%s\"", context, apn) && _parser.recv("OK"))
        ret = 0;
    at_mutex.unlock();
    return ret;
}

nsapi_error_t MTSASInterface::init()
{
    _parser.setTimeout(MTSAS_RESTART_TIMEOUT);
    at_mutex.lock();
    //Reboot the chip
    _parser.send("AT#REBOOT");
    _parser.recv("OK");
    _parser.setTimeout(MTSAS_MISC_TIMEOUT);
    //Wait for response after reboot
    for (int i = 0; i < 10; i++){
        if (_parser.send("AT") && _parser.recv("OK")){
            break;
        }   
    }

    //Device name
    _parser.send("AT+CGMM");
    _parser.recv("OK");
    _parser.send("AT+CGMM");
    _parser.recv("OK");
    at_mutex.unlock();
    return 0;
}

nsapi_error_t MTSASInterface::connect(const char *apn,
            const char *username, const char *password)
{
    return (init() || set_credentials(apn) || connect()) ? NSAPI_ERROR_NO_CONNECTION : 0;
}

bool MTSASInterface::registered()
{
    //Get the network registation
    int stat = NOT_REGISTERED;
    at_mutex.lock();    
    _parser.send("AT+CREG?");
    _parser.recv("+CREG:%*d,%d", &stat);
    _parser.recv("OK"); 
    //Keep trying if we are searching for a registration
    while(stat == SEARCHING){
        _parser.send("AT+CREG?");
        _parser.recv("+CREG:%*d,%d", &stat);
        _parser.recv("OK");
    }
    at_mutex.unlock();
    return (stat == REGISTERED || stat == ROAMING);
}

bool MTSASInterface::set_ip_addr()
{
    char* ip_buff = (char*)malloc(256);
    bool res = false; 
    at_mutex.lock();  
    //Try a few times to get an IP address 
    for (int i=0; i<5; i++){
        res  = _parser.send("AT#SGACT=%d,1", context) && 
               _parser.recv("#SGACT: %s%*[\r]%*[\n]", ip_buff) &&
               _parser.recv("OK");
        if(res)
            break;
    } 
    at_mutex.unlock();
    res = res && _ip_address.set_ip_address(ip_buff);
    free(ip_buff);
    return res;
}
 
nsapi_error_t MTSASInterface::connect()
{
    if (!registered() || !set_ip_addr()){
        return NSAPI_ERROR_DEVICE_ERROR;
    }
    return 0;
}

nsapi_error_t MTSASInterface::disconnect() 
{
    //Deactivate PDP context (frees any network resources associated with context)
    at_mutex.lock();    
    int ret = (_parser.send("AT#SGACT=%d,0",context) && _parser.recv("OK")) ? 0 : NSAPI_ERROR_DEVICE_ERROR; 
    at_mutex.unlock();
    return ret;
}

const char *MTSASInterface::get_ip_address()
{
    if(_ip_address.get_ip_address() == NULL){
        set_ip_addr();
    }
    return _ip_address.get_ip_address();
}

const char *MTSASInterface::get_mac_address()
{
    return 0;
}

nsapi_error_t MTSASInterface::gethostbyname(const char* name, SocketAddress *address, nsapi_version_t version)
{ 
    char* ip_buff = (char*)malloc(256);
    //Execute DNS query
    at_mutex.lock();
    int ret = 0;
    if (!_parser.send("AT#QDNS=%s",name) || !_parser.recv("#QDNS:%*[^,],\"%[^\"]\"%*[\r]%*[\n]", ip_buff) || !_parser.recv("OK")){
        ret = NSAPI_ERROR_DEVICE_ERROR;
    } 
    else{
        address->set_ip_address(ip_buff);
    }
    at_mutex.unlock();
    free(ip_buff);
    return ret; 
}
 
NetworkStack *MTSASInterface::get_stack()
{
    return this;
}

struct mtsas_socket {
    nsapi_protocol_t proto;
    bool connected;
    int port;
    int id;
    SocketAddress addr;
};

int MTSASInterface::socket_open(void **handle, nsapi_protocol_t proto)
{
    // Look for unused socket
    int id = -1;
    for (int i = 0; i < MTSAS_SOCKET_COUNT; i++) {
        if (!_socket_ids[i]){
            // IDS 1-6 valid
            id = i+1;
            // Mark the socket in use
            _socket_ids[i] = true;
            break;
        }
    }
    if (id == -1){
        return NSAPI_ERROR_NO_SOCKET;
    }
    struct mtsas_socket *socket = new struct mtsas_socket;
    socket->id = id;
    socket->port = 1;
    socket->proto = proto;
    socket->connected = false;
    *handle = socket;
    return 0;
}

int MTSASInterface::socket_close(void *handle)
{
    struct mtsas_socket *socket = (struct mtsas_socket *)handle;
    //Issue socket close command
    at_mutex.lock();
    bool result = (_parser.send("AT#SH=%d",socket->id) && _parser.recv("OK"));   
    at_mutex.unlock();
    if (result){
        //Mark the socket not in use
        _socket_ids[socket->id-1] = false;
        delete socket;
        return 0;
    }
    return NSAPI_ERROR_DEVICE_ERROR;
}

int MTSASInterface::socket_bind(void *handle, const SocketAddress &address)
{
    return NSAPI_ERROR_UNSUPPORTED;
}
 
int MTSASInterface::socket_listen(void *handle, int backlog)
{
    return NSAPI_ERROR_UNSUPPORTED;
}

int MTSASInterface::socket_connect(void *handle, const SocketAddress &address)
{
    struct mtsas_socket *socket = (struct mtsas_socket *)handle;   
    if (socket->connected){
        return 0;
    }
    uint16_t typeSocket = (socket->proto == NSAPI_UDP) ? 1 : 0;
    at_mutex.lock();
    //Socket dial SD=[socket id], [UDP or TCP], [Remote port], [Remote addr]
    bool res =  (_parser.send("AT#SD=%d,%d,%d,\"%s\",0,1,1", socket->id, typeSocket, 
                 address.get_port(), address.get_ip_address()) &&
                _parser.recv("OK"));
    at_mutex.unlock();
    if (res){
        socket->connected = true;
        return 0;
    }
    return NSAPI_ERROR_DEVICE_ERROR;
}
 
int MTSASInterface::socket_accept(nsapi_socket_t server,
            nsapi_socket_t *handle, SocketAddress *address)
{
    return NSAPI_ERROR_UNSUPPORTED;
}

int MTSASInterface::socket_send(void *handle, const void *data, unsigned size)
{
    struct mtsas_socket *socket = (struct mtsas_socket *)handle;   
    int amnt_sent = -1;
    //Issue send command SSENDEXT=[socket id], [# bytes to send]
    at_mutex.lock();
    _parser.setTimeout(MTSAS_COMMUNICATION_TIMEOUT);
    if(_parser.send("AT#SSENDEXT=%d,%d",socket->id, size)){
        //OK to write message
        _parser.recv("> ");
        amnt_sent = _parser.write((char *)data, (int)size);
        _parser.recv("OK");
    }
    at_mutex.unlock();
    return amnt_sent;
}

int MTSASInterface::socket_recv(void *handle, void *data, unsigned size)
{
    struct mtsas_socket *socket = (struct mtsas_socket *)handle;   
    int amnt_rcv = -1;
    //Paramater size is desired # bytes, recv_size is bytes actually on socket
    int recv_size = 0;
    at_mutex.lock();
    _parser.setTimeout(MTSAS_COMMUNICATION_TIMEOUT);
    //Issue send command SRECV=[socket id], [# bytes to recv]
    if(_parser.send("AT#SRECV=%d,%d",socket->id, size) && _parser.recv("#SRECV:%d,%d%*[\r]%*[\n]", socket->id, &recv_size)){
        amnt_rcv = _parser.read((char *)data, (int)recv_size);
        _parser.recv("OK");
    }
    at_mutex.unlock();
    if (amnt_rcv == -1 || amnt_rcv == 0) {
        return NSAPI_ERROR_WOULD_BLOCK;
    }
    return amnt_rcv;
}

int MTSASInterface::socket_sendto(void *handle, const SocketAddress &address, const void *data, unsigned size)
{
    struct mtsas_socket *socket = (struct mtsas_socket *)handle;
    if (socket->connected && socket->addr != address) {
        if (socket_close(&handle) != 0) {
            return NSAPI_ERROR_DEVICE_ERROR;
        }
        socket->connected = false;
    }

    if (!socket->connected) {
        int err = socket_connect(socket, address);
        if (err < 0) {
            return err;
        }
        socket->addr = address;
    }
    return socket_send(socket, data, size);
}

int MTSASInterface::socket_recvfrom(void *handle, SocketAddress *address, void *buffer, unsigned size)
{
    struct mtsas_socket *socket = (struct mtsas_socket *)handle;   
    int ret = socket_recv(socket, (char *)buffer, size);
    if (ret >= 0 && address) {
        *address = socket->addr;
    }
    return ret;
}

void MTSASInterface::socket_attach(void *handle, void (*callback)(void *), void *data)
{
    struct mtsas_socket *socket = (struct mtsas_socket *)handle;   
    _cbs[socket->id-1].callback = callback;
    _cbs[socket->id-1].data = data;
}

void MTSASInterface::rx_sem_release(){
    rx_sem.release();
}

void MTSASInterface::handle_event(){
    while(true){
        //Wait for signal from serial RX
        rx_sem.wait();
        //Unlock the mutex controlling AT command execution
        at_mutex.lock();
        _parser.setTimeout(0);
        //Check for SRING incoming data
        bool res = (_parser.recv("SRING:%*d"));       
        _parser.setTimeout(MTSAS_MISC_TIMEOUT);
        at_mutex.unlock();
        if(res){
            //Raise an event if the socket has data
            event();
        }
    }
}

void MTSASInterface::event() {
    for (int i = 0; i < MTSAS_SOCKET_COUNT; i++){
        if (_cbs[i].callback) {
            _cbs[i].callback(_cbs[i].data);
        }
    }
}

////////////////////////////////////////////////////////////////////////
//Cell module methods
////////////////////////////////////////////////////////////////////////
void MTSASInterface::get_imei(char* imei){
    at_mutex.lock();
    _parser.send("AT#CGSN");
    _parser.recv("#CGSN: %s%*[\r]%*[\n]", imei);
    at_mutex.unlock();
}

void MTSASInterface::sms_attach(void (*callback)(char*)){
    _sms_cb = callback;
    sms_listen();
} 

void MTSASInterface::sms_rx_sem_release(){  
    sms_rx_sem.release();
}


void MTSASInterface::sms_listen(){
    at_mutex.lock();
    //Receive texts in text mode (unencoded)
    _parser.send("AT+CMGF=1");
	_parser.recv("OK");
    //Show all values in result codes
	_parser.send("AT+CSDH=1");
    _parser.recv("OK");
    //Specify the buffering of unsolicited text notifications to go to TA
    //and also that the text message be displayed with the notification
    _parser.send("AT+CNMI=2,2");
    _parser.recv("OK");
    at_mutex.unlock();
    //Signal thread when there is a serial rx
    _serial.attach(callback(this, &MTSASInterface::sms_rx_sem_release));
    sms_event_thread.start(callback(this, &MTSASInterface::handle_sms_event));
}

void MTSASInterface::handle_sms_event(){
    while(true){
        //Wait for signal from serial RX
        sms_rx_sem.wait();
        //Unlock the mutex controlling AT command execution
        at_mutex.lock();
        _parser.setTimeout(0);
        //Check for +CMT incoming message
	    char msg[256];
	    int length;
        //Parse notification for the length of the message
        //TODO: Parse and handle phone number, timestamp, etc
        bool res = _parser.recv("+CMT: %*[^,],%*[^,],%*[^,],%*[^,],%*[^,],%*[^,],%*[^,],%*[^,],%*[^,],%*[^,],%d%*[\r]%*[\n]", &length);
        if(res){
            //Max message 256
	        if(length > 256){
                length = 255;
            }
            //Read the message
            _parser.read(msg, length);
            _parser.setTimeout(MTSAS_MISC_TIMEOUT);
            at_mutex.unlock();
		    msg[length]='\0';
            //Raise an event if the socket has data
            _sms_cb(msg);
        }
    }
}

////////////////////////////////////////////////////////////////////////
//GPS module methods
////////////////////////////////////////////////////////////////////////
int MTSASInterface::get_gps_state(){
    int state;
    at_mutex.lock();
    //Query the gps status
    _parser.send("AT$GPSP?");
    _parser.recv("$GPSP: %d", &state);
    _parser.recv("OK");
    at_mutex.unlock();
    return state;
}

bool MTSASInterface::set_gps_state(int state){
    bool res = true;
    //Check if the gos is already in the requested state
    if(get_gps_state() != state){
        at_mutex.lock();
        //Set gps state
        res = _parser.send("AT$GPSP=%d", state) && _parser.recv("OK");
        at_mutex.unlock();
    }
    return res;
}

int find_dir(char* coord){
    int i = 0;
    //Look for the cardinal direction in the coordinate
    while (coord[i] != 'W' && coord[i] != 'N' && coord[i] != 'S' && coord[i] != 'E'){
        i++;
    }
    char dir = coord[i];
    /**
    According to ISO 6709:
        North latitude is positive
        East longitude is positive
    **/
    return (dir == 'W' || dir == 'S') ? -1 : 1;
}

void format_data(gps_data* data){
    /**
    Response from the radio is:
    <latitude> - format is ddmm.mmmm N/S (referred to GGA sentence)
    where:
    dd - degrees
    00..90
    mm.mmmm - minutes
    00.0000..59.9999
    N/S: North / South
    <longitude> - format is dddmm.mmmm E/W (referred to GGA sentence)
    where:
    ddd - degrees
    000..180
    mm.mmmm - minutes
    00.0000..59.9999
    E/W: East / West
    

    This function converts this to ISO 6709 GPS standard
    **/
    char lat_deg[2];
    char long_deg[3];
    char lat_min[6];
    char long_min[6];
    strncpy(lat_deg, data->latitude, 2);
    strncpy(long_deg, data->longitude, 3);
    strncpy(lat_min, data->latitude+2, 6);
    strncpy(long_min, data->longitude+3, 6);
    //Find the cardinal direction of the coordinate
    int lat_dir = find_dir(data->latitude);
    int long_dir = find_dir(data->longitude);
    memset(&data->longitude[0], 0, sizeof(data->longitude));
    memset(&data->latitude[0], 0, sizeof(data->latitude));
    //Convert string values to floats and compute ISO 6709 GPS coordinates
    float lat = lat_dir * (std::atof(lat_deg) + std::atof(lat_min)/60);
    float lon = long_dir * (std::atof(long_deg) + std::atof(long_min)/60);
    sprintf(data->latitude, "%f", lat);
    sprintf(data->longitude, "%f", lon);
}
    
gps_data MTSASInterface::get_gps_location(const char* lat_default, const char* lon_default){
    //enable GPS
    set_gps_state(1); 
    struct gps_data data = {"None", "None", "None", "None"};
    //Start querying location
    bool res = _parser.send("AT$GPSACP") && _parser.recv("OK");
    if (!res){
        //Return immediately if we cannot turn on the GPS module
        return data;
    }
    Timer t;
    t.start(); 
    bool fix = false;
    //Timeout if we do not receive a gps location in two minutes
    while(!fix && t.read()<120){
        at_mutex.lock();
        //Query the GPS location
        _parser.send("AT$GPSACP");
        //Parse the radio response
        fix = _parser.recv("$GPSACP:%[^,],%[^,],%[^,],%*[^,],%[^,],%*[^,],%*[^,],%*[^,],%*[^,],%*[^,],%*[^\n]", data.UTC, data.latitude, data.longitude, data.altitude);
        _parser.recv("OK");
        at_mutex.unlock();
        wait(4);
    }
    set_gps_state(0);
    if(fix){
        //Format the coordinated to ISO 6709 if we got a GPS fix
        format_data(&data);
    }
    else{
        //Use default values if we did not get a fix
        strcpy(data.latitude, lat_default);
        strcpy(data.longitude, lon_default);
    }
    return data;
}
