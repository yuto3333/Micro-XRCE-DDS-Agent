// Copyright 2018 Proyectos y Sistemas de Mantenimiento SL (eProsima).
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <uxr/agent/transport/can/CanAgentLinux.hpp>
#include <uxr/agent/utils/Conversion.hpp>
#include <uxr/agent/logger/Logger.hpp>

#include <unistd.h>

#include <net/if.h>
#include <sys/ioctl.h>

#include <linux/can.h>
#include <linux/can/raw.h>

#include <sys/time.h>

/*---ここから追加のマクロ，グローバル変数---*/
/*状態*/
#define DDSXRCESTATE_NONE 0
#define DDSXRCESTATE_TOPIC 1
#define DDSXRCESTATE_SUBSCRIBER 2
#define DDSXRCESTATE_DATAREADER 3
#define DDSXRCESTATE_LAST 4

/*submessageIdの種類*/
#define SUBMESSAGEID_CREATECLIENT 0x00
#define SUBMESSAGEID_CREATE 0x01
#define SUBMESSAGEID_READ_DATA 0x08
#define SUBMESSAGEID_DATA 0x09
#define SUBMESSAGEID_ACKNACK 0x0A
#define SUBMESSAGEID_FRAGMENT 0x0D

/*Object Kindの種類*/
#define OBKIND_PARTICIPANT 0x01
#define OBKIND_TOPIC 0x02
#define OBKIND_SUBSCRIBER 0x04
#define OBKIND_DATAREADER 0x06

static uint8_t* pdata;
static int state = DDSXRCESTATE_NONE;


/*fragmentで用いる*/
static uint8_t cdata[256];
static int cdataindex = 5;
static int fragment_flag = 0; /*Fragmentの途中*/ /*0:途中でない,1:途中*/
static int fragment_first = 0; /*最初のFragment*/ /*0:最初,1:最初でない*/
#define LASTFRAGMENTFLAGS 0x03

/*DDSXRCESTATE_TOPICで用いる*/
#define TOPICNAMELEN_MAX 256
static char topicname[TOPICNAMELEN_MAX];
static int  topicnameindex = 0;
static int objectidtopic = -1;

/*DDSXRCESTATE_SUBSCRIBERで用いる*/
static int objectidsubscriber = -1;

/*DDSXRCESTATE_DATAREADERで用いる*/
#define NUM_TOPIC 3 /*トピックの数*/
static int prefixid = -1;
unsigned int objectid2canid[NUM_TOPIC];

/*計測で用いる*/
struct timeval start_time_recv;
struct timeval end_time_recv;
int recv_time;
int timecnt_rv;
struct timeval start_time_send;
struct timeval end_time_send;
int send_time;
int timedatacnt;
int timecnt_sd;



/*---ここまで追加のグローバル変数---*/

/*---ここから構造体---*/
typedef struct {
    char *topic_name;
    unsigned int topic_canid;
} NAME_CANID;

/*ユーザがトピック名，CAN-IDを設定*/
const NAME_CANID name_canid_table[] = {
    {
        "can_topic_1",
		0x00001101
    },
    {
        "can_topic_2",
		0x00001102
    },
    {
        "can_topic_3",
		0x00001103
    },
};
/*---ここまで構造体---*/

namespace eprosima {
namespace uxr {

CanAgent::CanAgent(
        char const* dev,
        uint32_t can_id,
        Middleware::Kind middleware_kind)
    : Server<CanEndPoint>{middleware_kind}
    , dev_{dev}
    , can_id_{can_id}
{
}

CanAgent::~CanAgent()
{
    try
    {
        stop();
    }
    catch (std::exception& e)
    {
        UXR_AGENT_LOG_CRITICAL(
            UXR_DECORATE_RED("error stopping server"),
            "exception: {}",
            e.what());
    }
}

bool CanAgent::init()
{
    static int enable_canfd = 1;
    bool rv = false;

    poll_fd_.fd = socket(PF_CAN, SOCK_RAW, CAN_RAW);

    if (-1 != poll_fd_.fd)
    {
        struct sockaddr_can address {};
        struct ifreq ifr;

        // Get interface index by name
        strcpy(ifr.ifr_name, dev_.c_str());
        ioctl(poll_fd_.fd, SIOCGIFINDEX, &ifr);

        memset(&address, 0, sizeof(address));
        address.can_family = AF_CAN;
        address.can_ifindex = ifr.ifr_ifindex;

        if (-1 != bind(poll_fd_.fd,
                reinterpret_cast<struct sockaddr*>(&address),
                sizeof(address)))
        {
            // Enable CAN FD
            if (-1 != setsockopt(poll_fd_.fd, SOL_CAN_RAW, CAN_RAW_FD_FRAMES,
                    &enable_canfd, sizeof(enable_canfd)))
            {
                poll_fd_.events = POLLIN;
                rv = true;

                UXR_AGENT_LOG_INFO(
                    UXR_DECORATE_GREEN("running..."),
                    "device: {}, fd: {}",
                    dev_, poll_fd_.fd);


                // TODO: add filter for micro-ROS devices
            }
            else
            {
                UXR_AGENT_LOG_ERROR(
                    UXR_DECORATE_RED("Enable CAN FD failed"),
                    "device: {},errno: {}",
                    dev_, errno);
            }
        }
        else
        {
            UXR_AGENT_LOG_ERROR(
                UXR_DECORATE_RED("SocketCan bind error"),
                "device: {}, errno: {}",
                dev_, errno);
        }
    }
    else
    {
        UXR_AGENT_LOG_ERROR(
            UXR_DECORATE_RED("SocketCan error"),
            "errno: {}",
            errno);
    }

    return rv;
}

bool CanAgent::fini()
{
    if (-1 == poll_fd_.fd)
    {
        return true;
    }

    bool rv = false;
    if (0 == ::close(poll_fd_.fd))
    {
        UXR_AGENT_LOG_INFO(
            UXR_DECORATE_GREEN("server stopped"),
            "fd: {}, device: {}",
            poll_fd_.fd, dev_);
        rv = true;
    }
    else
    {
        UXR_AGENT_LOG_ERROR(
            UXR_DECORATE_RED("close server error"),
            "fd: {}, device: {}, errno: {}",
            poll_fd_.fd, dev_, errno);
    }

    poll_fd_.fd = -1;
    return rv;
}

/*---ここからfragment関数---*/
int fragment(struct canfd_frame* frame){
	fragment_flag = 1;
	int i; /*cdataにコピーする際のi*/

	if (fragment_first == 0){
		for (i = 0; i < 5; i++){
			cdata[i] = 0x00; /*最初のFragmentはcdata[1]-cdata[4]に0を代入*/
		}
		fragment_first = 1;
	}

    if (frame->data[5] == SUBMESSAGEID_FRAGMENT){
        for (i = 9; i < (int)frame->len; i++){
            if ((i == 60) && (frame->data[i] == 0x00)){
                break;
            }
            cdata[cdataindex] = frame->data[i]; /*cdata[]にframe.data[]をコピーし，メッセージを繋げる*/
            cdataindex++;
	    }
    } else if (frame->data[5] == SUBMESSAGEID_READ_DATA){
        for (i = 29; i < (int)frame->len; i++){
            if ((i == 60) && (frame->data[i] == 0x00)){
                break;
            }
            cdata[cdataindex] = frame->data[i]; /*cdata[]にframe.data[]をコピーし，メッセージを繋げる*/
            cdataindex++;
	    }
    }

	if ((frame->data[5] == SUBMESSAGEID_FRAGMENT) && (frame->data[6] == LASTFRAGMENTFLAGS)){ /*最後のFragment*/

		fragment_first = 0;
		fragment_flag = 0;

		cdataindex = 5; /*dataindexの初期化*/

		/*それぞれの状態に遷移する*/
		if ((cdata[5] == SUBMESSAGEID_CREATECLIENT) || ((cdata[5] == SUBMESSAGEID_CREATE) && ((cdata[12] & 0x0F) == OBKIND_PARTICIPANT))){
			state = DDSXRCESTATE_NONE;
		} else if ((cdata[5] == SUBMESSAGEID_CREATE) && ((cdata[12] & 0x0F) == OBKIND_TOPIC)){
			state = DDSXRCESTATE_TOPIC;
		} else if ((cdata[5] == SUBMESSAGEID_CREATE) && ((cdata[12] & 0x0F) == OBKIND_SUBSCRIBER)){
			state = DDSXRCESTATE_SUBSCRIBER;
		} else if ((cdata[5] == SUBMESSAGEID_CREATE) && ((cdata[12] & 0x0F) == OBKIND_DATAREADER)){
			state = DDSXRCESTATE_DATAREADER;
		} else if ((cdata[5] == SUBMESSAGEID_READ_DATA) && (cdata[25] == SUBMESSAGEID_FRAGMENT)){
            state = DDSXRCESTATE_LAST;
        } else {
			
		}
	}

	return fragment_flag;

}
/*---ここまでfragment関数---*/

/*
 * submessageId   |  Object Kind        |   flags
 * ====================================================
 *    pdata[5]    |    pdata[12] & 0x0F |  frame.data[6]
 *    cdata[5]    |    cdata[12] & 0x0F |
 */
bool CanAgent::recv_message(
        InputPacket<CanEndPoint>& input_packet,
        int timeout,
        TransportRc& transport_rc)
{
    bool rv = false;
    struct canfd_frame frame = {};

    int poll_rv = poll(&poll_fd_, 1, timeout);

    if (0 < poll_rv)
    {
        gettimeofday(&start_time_recv, NULL);/*計測開始*/
        if (0 < read(poll_fd_.fd, &frame, sizeof(struct canfd_frame)))
        {
            // Omit EFF, RTR, ERR flags (Assume EFF on CAN FD)
            uint32_t can_id = frame.can_id & CAN_ERR_MASK & 0xFFFFFFF0;
            //printf("frame.can_id:%x", frame.can_id);
            size_t len = frame.data[0];   // XRCE payload lenght

            if (len > (CANFD_MTU - 1))
            {
                // Overflow MTU (63 bytes)
                return false;
            }

            input_packet.message.reset(new InputMessage(&frame.data[1], len));
            input_packet.source = CanEndPoint(can_id);
            rv = true;

            uint32_t raw_client_key;
            if (Server<CanEndPoint>::get_client_key(input_packet.source, raw_client_key))
            {
                UXR_AGENT_LOG_MESSAGE(
                    UXR_DECORATE_YELLOW("[==>> CAN <<==]"),
                    raw_client_key,
                    input_packet.message->get_buf(),
                    input_packet.message->get_len());
            }
        }
        else
        {
            transport_rc = TransportRc::server_error;
        }
        /*---ここから状態遷移プログラム---*/
        bool nofragment_data;
        int i;

        pdata = frame.data;

        /*---ここから状態：fragment---*/
        if ((pdata[5] == SUBMESSAGEID_FRAGMENT) || ((pdata[5] == SUBMESSAGEID_READ_DATA) && (pdata[25] == SUBMESSAGEID_FRAGMENT))){

            /*Fragmentの場合の処理*/
            fragment_flag = fragment(&frame);
            if (fragment_flag == 0) {
                pdata = cdata;
            }
            nofragment_data = false;
        } else {
            nofragment_data = true;
        }
        /*---ここまで状態：fragment---*/

        /*---ここからその他の状態---*/
        if (nofragment_data || fragment_flag == 0){

            if (pdata[5] == SUBMESSAGEID_CREATECLIENT){
                state = DDSXRCESTATE_NONE;
            }

            /*---ここから状態：DDSXRCESTATE_NONE---*/
            if (state == DDSXRCESTATE_NONE){
                //printf("state=NONE:%d\n", state);

                if ((pdata[5] == SUBMESSAGEID_CREATECLIENT) || (pdata[5] == SUBMESSAGEID_ACKNACK) || ((pdata[5] == SUBMESSAGEID_CREATE) && ((pdata[12] & 0x0F) == OBKIND_PARTICIPANT))){
                    state = DDSXRCESTATE_NONE;
                } else if ((pdata[5] == SUBMESSAGEID_CREATE) && ((pdata[12] & 0x0F) == OBKIND_TOPIC)){
                    state = DDSXRCESTATE_TOPIC;
                } else {
                    /*err*/
                }
            }
            /*---ここまで状態：DDSXRCESTATE_NONE---*/

            /*---ここから状態：DDSXRCESTATE_TOPIC---*/
            if (state == DDSXRCESTATE_TOPIC){
                //printf("state=TOPIC:%d\n", state);

                if ((pdata[5] == SUBMESSAGEID_CREATE) && ((pdata[12] & 0x0F) == OBKIND_TOPIC)){
                    /*トピック名の抽出*/
                    for (i = 28; i < TOPICNAMELEN_MAX; i++){

                        if (pdata[i] == 0x00){
                            //printf("<=topicname\n");
                            topicnameindex = 0;
                            break;
                        }
                        topicname[topicnameindex] = pdata[i];
                        //printf("%c ", topicname[topicnameindex]);
                        topicnameindex++;

                    }

                    objectidtopic = pdata[12];
                    //printf("objectidtopic = %x\n",objectidtopic);
                    state = DDSXRCESTATE_SUBSCRIBER;
                } else {
                    /*err*/
                }

            }
            /*---ここまで状態：DDSXRCESTATE_TOPIC---*/

            /*---ここから状態：DDSXRCESTATE_SUBSCRIBER---*/
            if (state == DDSXRCESTATE_SUBSCRIBER){
                //printf("state=SUBSCRIBER:%d\n", state);

                if ((pdata[5] == SUBMESSAGEID_CREATE) && ((pdata[12] & 0x0F) == OBKIND_SUBSCRIBER)){
                    objectidsubscriber = pdata[12];
                    //printf("objectidsubscriber = %x\n", objectidsubscriber);
                    state = DDSXRCESTATE_DATAREADER;
                } else if ((pdata[5] == SUBMESSAGEID_ACKNACK) || ((pdata[5] == SUBMESSAGEID_CREATE) && ((pdata[12] & 0x0F) == OBKIND_TOPIC))){
                    state = DDSXRCESTATE_SUBSCRIBER;
                } else {
                    /*err*/
                }

            }
            /*---ここまで状態：DDSXRCESTATE_SUBSCRIBER---*/

            /*---ここから状態：DDSXRCESTATE_DATAREADER---*/
            if (state == DDSXRCESTATE_DATAREADER){
                //printf("state=DATAREADER:%d\n", state);

                if ((pdata[5] == SUBMESSAGEID_CREATE) && ((pdata[12] & 0x0F) == OBKIND_DATAREADER) && (pdata[13] == OBKIND_DATAREADER)){

                    if ((objectidtopic == pdata[22]) && (objectidsubscriber == pdata[37])){

                        prefixid = pdata[12] >> 4;
                        //printf("prefixid:%x\n", prefixid);

                        if (strlen(topicname) == strlen(name_canid_table[prefixid].topic_name)){
                            //printf("strlen(topicname) : %ld\n", strlen(topicname));
                            //printf("strlen(name_canid_table[prefixid].topic_name) : %ld\n", strlen(name_canid_table[prefixid].topic_name));

                            if (strcmp(topicname, name_canid_table[prefixid].topic_name) == 0){
                                //printf("strcmp : %d\n", strcmp(topicname, name_canid_table[prefixid].topic_name));

                                objectid2canid[prefixid] = name_canid_table[prefixid].topic_canid; /*設定したCAN-IDの取得*/
                                //printf("CANID:%x\n", objectid2canid[prefixid]);

                            }
                            
                        
                        } else {
                            /*トピック名の長さが異なっている*/
                        }
                    } else {
                        /*各IDが異なっている*/
                    }

                    /*各変数の初期化*/
                    prefixid = -1;
                    objectidtopic = -1;
                    objectidsubscriber = -1;
                    topicnameindex = 0;

                    state = DDSXRCESTATE_LAST;

                } else if(((pdata[5] == SUBMESSAGEID_CREATE) && ((pdata[12] & 0x0F) == OBKIND_SUBSCRIBER)) || (pdata[5] == SUBMESSAGEID_ACKNACK)){

                    state = DDSXRCESTATE_DATAREADER;

                }
            }
            /*---ここまで状態：DDSXRCESTATE_DATAREADER---*/

            /*---ここから状態：DDSXRCESTATE_LAST---*/
            if (state == DDSXRCESTATE_LAST){
                //printf("state=LAST:%d\n", state);
                if ((pdata[5] == SUBMESSAGEID_READ_DATA) && (pdata[25] != SUBMESSAGEID_FRAGMENT)){
                    /*生成メッセージの処理終了*/
                } else if ((pdata[5] == SUBMESSAGEID_CREATE) && ((pdata[12] & 0x0F) == OBKIND_DATAREADER) && (pdata[13] == OBKIND_DATAREADER)){
                    state = DDSXRCESTATE_LAST;
                } else {
                    state = DDSXRCESTATE_NONE;
                }
            }
            /*---ここまで状態：DDSXRCESTATE_LAST---*/
            
        }
        /*---ここまでその他の状態---*/
        /*---ここまで状態遷移プログラム---*/
        gettimeofday(&end_time_recv, NULL);/*計測終了*/
        recv_time = ((end_time_recv.tv_sec * 1000000) + end_time_recv.tv_usec) - ((start_time_recv.tv_sec * 1000000) + start_time_recv.tv_usec);
        if (timecnt_rv < 100){
            printf("recv_time:%d\n", recv_time);
            timecnt_rv++;
        }
    }
    else
    {
        transport_rc = (poll_rv == 0) ? TransportRc::timeout_error : TransportRc::server_error;
    }  
    //printf("frame.can_id:%x\n", frame.can_id);
    //printf("end_time_recv.tv_sec:%ld\n", end_time_recv.tv_sec);
    //printf("end_time_recv.tv_usec:%ld\n", end_time_recv.tv_usec);
    //printf("start_time_recv.tv_sec:%ld\n", start_time_recv.tv_sec);
    //printf("start_time_recv.tv_usec:%ld\n", start_time_recv.tv_usec);
    return rv;
    
}

#define AGENTCANID_END 0x00000100

bool CanAgent::send_message(
        OutputPacket<CanEndPoint> output_packet,
        TransportRc& transport_rc)
{
    bool rv = false;
    struct canfd_frame frame = {};
    struct pollfd poll_fd_write_;
    size_t packet_len = output_packet.message->get_len();

    if (packet_len > (CANFD_MTU - 1))
    {
        // Overflow MTU (63 bytes)
        return 0;
    }

    poll_fd_write_.fd = poll_fd_.fd;
    poll_fd_write_.events = POLLOUT;
    int poll_rv = poll(&poll_fd_write_, 1, 0);

    if (0 < poll_rv)
    {
        gettimeofday(&start_time_send, NULL);/*計測開始*/
        frame.can_id = (output_packet.destination.get_can_id() + AGENTCANID_END) | CAN_EFF_FLAG;
        frame.data[0] = (uint8_t) packet_len;   // XRCE payload lenght
        frame.len = (uint8_t) (packet_len + 1);   // CAN frame DLC

        memcpy(&frame.data[1], output_packet.message->get_buf(), packet_len);
        
        /*---ここからCANIDの割り当て---*/

//        if (frame.data[5] == SUBMESSAGEID_DATA){
//            
//            frame.can_id = objectid2canid[frame.data[12] >> 4] | CAN_EFF_FLAG; /*CAN-IDの割り当て*/
//
//	    }
        
        bool nofragment_data;

        pdata = frame.data;

        if (pdata[5] == SUBMESSAGEID_FRAGMENT){
            fragment_flag = fragment(&frame);
            if (fragment_flag == 0){
                pdata = cdata;
            }
            nofragment_data = false;
        } else {
            nofragment_data = true;
        }

        if (nofragment_data || fragment_flag == 0){

            if (pdata[5] == SUBMESSAGEID_DATA){

                frame.can_id = objectid2canid[pdata[12] >> 4] | CAN_EFF_FLAG; /*CAN-IDの割り当て*/

            }
        }
        /*---ここまでCANIDの割り当て---*/

        if (0 < ::write(poll_fd_.fd, &frame, sizeof(struct canfd_frame)))
        {
            rv = true;

            uint32_t raw_client_key;
            if (Server<CanEndPoint>::get_client_key(output_packet.destination, raw_client_key))
            {
                UXR_AGENT_LOG_MESSAGE(
                    UXR_DECORATE_YELLOW("[** <<CAN>> **]"),
                    raw_client_key,
                    output_packet.message->get_buf(),
                    packet_len);
            }
        }
        else
        {
            // Write failed
            transport_rc = TransportRc::server_error;
        }

        gettimeofday(&end_time_send, NULL);/*計測終了*/
        send_time = ((end_time_send.tv_sec * 1000000) + end_time_send.tv_usec) - ((start_time_send.tv_sec * 1000000) + start_time_send.tv_usec);
        if (timecnt_sd < 100){
            printf("send_time:%d\n", send_time);
            timecnt_sd ++;
        }

    }
    else
    {
        // Can device is busy
        transport_rc = TransportRc::server_error;
    }

    //printf("end_time_send.tv_sec:%ld\n", end_time_send.tv_sec);
    //printf("end_time_send.tv_usec:%ld\n", end_time_send.tv_usec);
    //printf("start_time_send.tv_sec:%ld\n", start_time_send.tv_sec);
    //printf("start_time_send.tv_usec:%ld\n", start_time_send.tv_usec);
    return rv;

}

bool CanAgent::handle_error(
        TransportRc /*transport_rc*/)
{
    return fini() && init();
}

} // namespace uxr
} // namespace eprosima
