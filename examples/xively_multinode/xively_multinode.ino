/*
  NanodeRF_multinode

  Relay's data recieved from wireless nodes to emoncms
  Decodes reply from server to set software real time clock
  Relay's time data to emonglcd - and any other listening nodes.
  Looks for 'ok' reply from request to verify data reached emoncms

  emonBase Documentation: http://openenergymonitor.org/emon/emonbase

  Authors: Trystan Lea and Glyn Hudson
  Part of the: openenergymonitor.org project
  Licenced under GNU GPL V3
  http://openenergymonitor.org/emon/license

  EtherCard Library by Jean-Claude Wippler and Andrew Lindsay
  JeeLib Library by Jean-Claude Wippler

  THIS SKETCH REQUIRES:
  
  Libraries in the standard arduino libraries folder:
	- JeeLib		https://github.com/jcw/jeelib
	- EtherCard		https://github.com/jcw/ethercard/

  Other files in project directory (should appear in the arduino tabs above)
	- decode_reply.ino
	- dhcp_dns.ino
*/

#define UNO       //anti crash wachdog reset only works with Uno (optiboot) bootloader, comment out the line if using delianuova

#include "RF12uiot.h" 
#include <avr/wdt.h>

#define MYNODE 16            
#define freq RF12_868MHZ     // frequency
#define group 210            // network group

// Change these settings to match your feed and api key
#define FEED "307112517"
#define APIKEY "dqjh5xr8iE4xWM5TapGUHq2FlsGN1sVwYZMICGOfh0Wspmb3"


//---------------------------------------------------------------------
// The PacketBuffer class is used to generate the json string that is send via ethernet - JeeLabs
//---------------------------------------------------------------------
class PacketBuffer : public Print {
public:
    PacketBuffer () : fill (0) {}
    const char* buffer() { return buf; }
    byte length() { return fill; }
    void reset()
    { 
      memset(buf,NULL,sizeof(buf));
      fill = 0; 
    }
    virtual size_t write (uint8_t ch)
        { if (fill < sizeof buf) buf[fill++] = ch; }
    byte fill;
    char buf[150];
    private:
};
PacketBuffer str;

//--------------------------------------------------------------------------
// Ethernet
//--------------------------------------------------------------------------
#include <EtherCard.h>		//https://github.com/jcw/ethercard 



// ethernet interface mac address, must be unique on the LAN
static byte mymac[] = { 0x42,0x31,0x42,0x21,0x30,0x31 };

// 1) Set this to the domain name of your hosted emoncms - leave blank if posting to IP address 
char website[] PROGMEM = "api.xively.com";

// or if your posting to a static IP server:
static byte hisip[] = { 192,168,1,10 };

// change to true if you would like the sketch to use hisip
boolean use_hisip = false;  

// 2) If your emoncms install is in a subdirectory add details here i.e "/emoncms3"
char basedir[] = "";

//IP address of remote sever, only needed when posting to a server that has not got a dns domain name (staticIP e.g local server) 
byte Ethernet::buffer[700];
static uint32_t timer;
Stash stash;

const int redLED = 5;                     // NanodeRF RED indicator LED
//const int redLED = 17;  		  // Open Kontrol Gateway LED indicator
const int greenLED = 5;                   // NanodeRF GREEN indicator LED

int ethernet_error = 0;                   // Etherent (controller/DHCP) error flag
int rf_error = 0;                         // RF error flag - high when no data received 
int ethernet_requests = 0;                // count ethernet requests without reply                 

int dhcp_status = 0;
int dns_status = 0;

int data_ready=0;                         // Used to signal that emontx data is ready to be sent
unsigned long last_rf;                    // Used to check for regular emontx data - otherwise error
unsigned long last_send;                   // Last send to cosm
char line_buf[50];                        // Used to store line of http reply header

byte havedata=0;

unsigned long time60s = -50000;
//**********************************************************************************************************************
// SETUP
//**********************************************************************************************************************
void setup () {
  
  //Nanode RF LED indictor  setup - green flashing means good - red on for a long time means bad! 
  //High means off since NanodeRF tri-state buffer inverts signal 
  pinMode(redLED, OUTPUT); digitalWrite(redLED,LOW);            
  pinMode(greenLED, OUTPUT); digitalWrite(greenLED,LOW);       
  delay(100); digitalWrite(redLED,HIGH);                          // turn off redLED

  //For use with the modified JeeLib library to enable setting RFM12B SPI CS pin in the sketch. Download from: https://github.com/openenergymonitor/jeelib 
  // rf12_set_cs(9);  //Open Kontrol Gateway	
  // rf12_set_cs(10); //emonTx, emonGLCD, NanodeRF, JeeNode

  rf12_initialize(MYNODE, freq,group);
  last_rf = millis()-40000;                                       // setting lastRF back 40s is useful as it forces the ethernet code to run straight away

  
  delay(5000);
  Serial.begin(9600);
  Serial.println("\n[webClient]");

  if (ether.begin(sizeof Ethernet::buffer, mymac, 10) == 0) {	//for use with Open Kontrol Gateway 
  //if (ether.begin(sizeof Ethernet::buffer, mymac) == 0) {	//for use with NanodeRF
    Serial.println( "Failed to access Ethernet controller");
    ethernet_error = 1;  
  }

  dhcp_status = 0;
  dns_status = 0;
  ethernet_requests = 0;
  ethernet_error=0;
  rf_error=0;

   
//  digitalWrite(greenLED,HIGH);                                    // Green LED off - indicate that setup has finished 
 
  #ifdef UNO
  wdt_enable(WDTO_8S); 
  #endif;
  
  str.reset();
  havedata=0;
}

//**********************************************************************************************************************
// LOOP
//**********************************************************************************************************************
void loop () {
  #ifdef UNO
  wdt_reset();
  #endif

  dhcp_dns();   // handle dhcp and dns setup - see dhcp_dns tab
  
  // Display error states on status LED
  if (ethernet_error==1 || rf_error==1 || ethernet_requests > 0) digitalWrite(redLED,LOW);
    else digitalWrite(redLED,HIGH);

  //-----------------------------------------------------------------------------------------------------------------
  // 1) On RF recieve
  //-----------------------------------------------------------------------------------------------------------------
  if (rf12_recvDone()){      
      if (rf12_crc == 0 && (rf12_hdr & RF12_HDR_CTL) == 0)
      {
        int node_id = (rf12_hdr & 0x1F);
        byte n = rf12_len;
         
        for (byte i=0; i<n; i+=2)
        {         
          str.print("node_"); str.print(node_id); str.print("_"); str.print(i>>1); str.print(",");
          unsigned int num = ((unsigned char)rf12_data[i+1] << 8 | (unsigned char)rf12_data[i]);
          str.print(num); str.print("\r\n");
                    
        }
        havedata=1;
        data_ready=1;
        last_rf = millis(); 
        rf_error=0;
      }
  }

  //-----------------------------------------------------------------------------------------------------------------
  // 3) Send data via ethernet
  //-----------------------------------------------------------------------------------------------------------------
  ether.packetLoop(ether.packetReceive());
  
 
  if (data_ready) {
    
    
 // ------------------------------------------------------------------------
    // Upload to Cosm fails if freeCount of the stash drops to 0, so this hacky
    // thing checks the current value of freeCount, and if it's below some
    // arbitrary value it resets it.
    // ------------------------------------------------------------------------

    // first capture the freeCount
    int freeCount = stash.freeCount();

    // reset the stash if freeCount has dropped to 10
    if (freeCount < 10) {
      stash.initMap(56);
    }

    byte sd = stash.create();
    
    Serial.print("Data sent: "); Serial.println(str.buf); // print to serial json string

    stash.println(str.buf);    
    stash.save();

   // generate the header with payload - note that the stash size is used,
    // and that a "stash descriptor" is passed in as argument using "$H"
    Stash::prepare(PSTR("PUT http://$F/v2/feeds/$F.csv HTTP/1.0" "\r\n"
                        "Host: $F" "\r\n"
                        "X-ApiKey: $F" "\r\n"
                        "Content-Length: $D" "\r\n"
                        "\r\n"
                        "$H"),
                        website,
                        PSTR(FEED),
                        website,
                        PSTR(APIKEY),
                        stash.size(),
                        sd);

    // send the packet - this also releases all stash buffers once done
    ether.tcpSend();
    
    
    ethernet_requests ++;
    //ether.httpPost(PSTR("/v2/feeds/129468.csv?_method=put"), website, PSTR("X-ApiKey:BWBLI1IlK_4LIOc2EMxI9Bn_zmCSAKxCN2ZpNW5ld1Q3WT0g"), str.buf, my_callback);
    
    
    data_ready =0;
    str.reset();
    havedata=0;
    last_send=millis();
  }
  
 // if (ethernet_requests > 10) delay(10000); // Reset the nanode if more than 10 request attempts have been tried without a reply

}
//**********************************************************************************************************************

//-----------------------------------------------------------------------------------
// Ethernet callback
// recieve reply and decode
//-----------------------------------------------------------------------------------
static void my_callback (byte status, word off, word len) {
  Serial.println("Server Reply");
  get_header_line(1, off);
  Serial.println(line_buf);
  Serial.println(strlen(line_buf));   
  ethernet_requests = 0; ethernet_error = 0;

}
