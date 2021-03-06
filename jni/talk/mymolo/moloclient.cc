/*
 * moloclient.cc
 *
 *  Created on: 2012-5-20
 *      Author: gongchen
 */

#include "talk/mymolo/moloclient.h"
#include <android/log.h>    
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, "P2P", __VA_ARGS__)
#include <string>
#include <iostream>
#include <jni.h>   

#include "talk/mymolo/console.h"
#include "talk/base/helpers.h"
#include "talk/base/logging.h"
#include "talk/base/network.h"
#include "talk/base/socketaddress.h"
#include "talk/base/stringencode.h"
#include "talk/base/stringutils.h"
#include "talk/base/thread.h"
#include "talk/examples/call/status.h"
#include "talk/p2p/base/sessionmanager.h"       
#include "talk/p2p/base/p2ptransportchannel.h"
#include "talk/p2p/client/basicportallocator.h"
#include "talk/p2p/client/sessionmanagertask.h"
#include "talk/xmpp/constants.h"
#include "talk/xmpp/jid.h"
#include "talk/xmpp/xmppengine.h"
#include "talk/xmpp/xmppclient.h"
#include "talk/molo/moloxmpptask.h"

using namespace cricket;

namespace {

const char* DescribeStatus(buzz::Status::Show show, const std::string& desc) {
	switch (show) {
	case buzz::Status::SHOW_XA:
		return desc.c_str();
	case buzz::Status::SHOW_ONLINE:
		return "online";
	case buzz::Status::SHOW_AWAY:
		return "away";
	case buzz::Status::SHOW_DND:
		return "do not disturb";
	case buzz::Status::SHOW_CHAT:
		return "ready to chat";
	default:
		return "offline";
	}
}

std::string GetWord(const std::vector<std::string>& words, size_t index,
		const std::string& def) {
	if (words.size() > index) {
		return words[index];
	} else {
		return def;
	}
}

int GetInt(const std::vector<std::string>& words, size_t index, int def) {
	int val;
	if (words.size() > index && talk_base::FromString(words[index], &val)) {
		return val;
	} else {
		return def;
	}
}

} // namespace
 
struct MoloMessage : public talk_base::MessageData {
  explicit MoloMessage(std::string v) : value(v) {}
  virtual ~MoloMessage() {}

  std::string value;
};

enum {
	MSG_CREATECHANNEL = 1, MSG_DESTROYCHANNEL = 2,
};

void MoloClient::CreateP2P(const std::string& name, const std::string& line) {
    LOG(LS_WARNING)<<"Create p2p to : "<< line;
    this->name = name;
    this->setRemoteUser(line);
	worker_thread_->PostDelayed(500, this, MSG_CREATECHANNEL);
}
                        
void MoloClient::SendData(const char* data) {
	LOG(LS_INFO)<<"MoloClient::SendData!!"; 
	//const char* data = str.c_str();
	int len = static_cast<int>(strlen(data));
	this->channel->SendPacket(data, len);
}

void MoloClient::ParseLine(const std::string& line) {
	std::vector<std::string> words;
	int start = -1;
	int state = 0;
	for (int index = 0; index <= static_cast<int>(line.size()); ++index) {
		if (state == 0) {
			if (!isspace(line[index])) {
				start = index;
				state = 1;
			}
		} else {
			ASSERT(state == 1);
			ASSERT(start >= 0);
			if (isspace(line[index])) {
				std::string word(line, start, index - start);
				words.push_back(word);                                                    
				start = -1;
				state = 0;
			}
		}
	}

	// Global commands
	const std::string& command = GetWord(words, 0, "");
	if (command == "quit") {
		Quit();
	} else if (command == "call") {
		std::string to = GetWord(words, 1, "");
		MakeCallTo(to);
	} else if (command == "send") {
		buzz::Jid jid(words[1]);
		SendChat(words[1], words[2]);
	} else if (command == "p2p") {
		to = words[1];
		worker_thread_->PostDelayed(1000, this, MSG_CREATECHANNEL);
	} else if (command == "go") {
		std::string word =  words[1];
		const char* data = word.c_str();
		int len = static_cast<int>(strlen(data));
		channel->SendPacket(data, len);
	} else if (command == "stop") {

	}
}

void MoloClient::Quit() {
	if (this->channel != NULL || !this->channel_is_not_work_) {
		this->worker_thread_->Post(this, MSG_DESTROYCHANNEL);
	}

	if (this->channel_is_not_work_) {
		talk_base::Thread::Current()->Quit();
	}
}

MoloClient::MoloClient(buzz::XmppClient* xmpp_client) :
		xmpp_client_(xmpp_client), worker_thread_(NULL), incoming_call_(false) { 			
    xmpp_client_->SignalStateChange.connect(this, &MoloClient::OnStateChange);
	// LOGD("MoloClient clear");
	ready_candidates_.clear();
	remote_candidates_.clear();
    channel_is_not_work_ = true;  
	env = NULL;
}

MoloClient::~MoloClient() {
	delete worker_thread_;
}

void MoloClient::OnStateChange(buzz::XmppEngine::State state) {
	switch (state) {
	case buzz::XmppEngine::STATE_START:
		console_->PrintLine("connecting...");
		break;
	case buzz::XmppEngine::STATE_OPENING:
		console_->PrintLine("logging in...");
		break;
	case buzz::XmppEngine::STATE_OPEN:
		console_->PrintLine("logged in...");
	    InitMedia();
		break;

	case buzz::XmppEngine::STATE_CLOSED:
		buzz::XmppEngine::Error error = xmpp_client_->GetError(NULL);
		console_->PrintLine("logged out... %s", strerror(error).c_str());
		Quit();
		break;
	}
}

void MoloClient::InitMedia() {
	//init status.
	SetAvailable(getXmppClient()->jid(), &my_status_);
	stanza = new buzz::XmlElement(buzz::QN_PRESENCE);
	stanza->AddElement(new buzz::XmlElement(buzz::QN_STATUS));
	stanza->AddText(my_status_.status(), 1);
	getXmppClient()->SendStanza(stanza);

	worker_thread_ = new talk_base::Thread();
	// The worker thread must be started here since initialization of
	// the ChannelManager will generate messages that need to be
	// dispatched by it.
	worker_thread_->Start();
	// TODO: It looks like we are leaking many objects. E.g.
	// |network_manager_| is never deleted.
	network_manager_ = new talk_base::BasicNetworkManager();

	talk_base::SocketAddress stun_addr("211.155.229.32", 3478);

	port_allocator_ = new cricket::BasicPortAllocator(network_manager_,
			stun_addr, talk_base::SocketAddress("relay.google.com", 19295),
			talk_base::SocketAddress("relay.google.com", 19294),
			talk_base::SocketAddress("relay.google.com", 443));

	molo_xmpp_task_ = new cricket::MoloXmppTask(xmpp_client_);
	molo_xmpp_task_->SignalHandleCandidate.connect(this,
			&MoloClient::HandleCandidate);
	molo_xmpp_task_->SignalHandleChat.connect(this, &MoloClient::HandleChat);
	xmpp_client_->AddXmppTask(molo_xmpp_task_, buzz::XmppEngine::HL_ALL);

//	molo_xmpp_task_->Start();
//	session_manager_ = new cricket::SessionManager(
//	      port_allocator_, worker_thread_);
//	session_manager_->SignalRequestSignaling.connect(this,
//			&MoloClient::OnRequestSignaling);
//	session_manager_->SignalSessionCreate.connect(this,
//			&MoloClient::OnSessionCreate);
//	session_manager_->OnSignalingReady();
//
//	session_manager_task_ = new cricket::SessionManagerTask(xmpp_client_,
//			session_manager_);
//	session_manager_task_->EnableOutgoingMessages();
//	session_manager_task_->Start();

//	//todo Our Session Client
//	molo_client_ = new cricket::MyMoloSessionClient(xmpp_client_->jid(),
//			session_manager_);
//	molo_client_->SignalCallCreate.connect(this, &MoloClient::OnCallCreate);
//	molo_client_->SignalCallDestroy.connect(this, &MoloClient::OnCallDestroy);

	//create p2p channel and get candidate.
//	talk_base::Thread::Current()->Post(this, MSG_CREATECHANNEL);//Send(this, MSG_CREATECHANNEL);
//	worker_thread_->Send(this, MSG_CREATECHANNEL);


//	worker_thread_->PostDelayed(1000, this, MSG_CREATECHANNEL);
}

void MoloClient::OnMessage(talk_base::Message *msg) {
	switch (msg->message_id) {
	case (MSG_CREATECHANNEL): {
		LOG(LS_INFO)<<"CREATE-CHANNEL!!";       

		//MoloMessage* remote_user = static_cast<MoloMessage*>(msg->pdata); 
		//std::string user = static_cast<std::string>(remote_user->value);
		__android_log_print(ANDROID_LOG_WARN, "P2P", "******MoloClient::remote user is  = %s",this->to.c_str());
		//remote_user->value                
		//this->to = "gc2@molohui.com";
        LOG(LS_INFO)<<"TransportChannel name is :" << this->name; // ex . gc1@molohui.com
		channel = new cricket::P2PTransportChannel(this->name,
				"p2p-test", NULL, port_allocator_);
		channel->SignalRequestSignaling.connect(channel,
				&cricket::P2PTransportChannel::OnSignalingReady);
		channel->SignalCandidateReady.connect(this, &MoloClient::OnCandidate);
		channel->SignalReadPacket.connect(this, &MoloClient::OnReadPacket);
		channel->Connect();
		channel_is_not_work_ = false;
		break;
	}
	case (MSG_DESTROYCHANNEL): {
		if (channel != NULL) {
			channel->disconnect_all();
		}

		break;
	}
	}
}

void MoloClient::Input(const char * data, int len) {

}

void MoloClient::OnReadPacket(cricket::TransportChannel* channel,
		const char* data, size_t len) {
	LOG(LS_INFO) << "OnReadPacket!! \n";
	std::string word = data;
	LOG(LS_INFO) << "data is : " << word.substr(0, len) << "   length is: "
			<< len << "\n";    
	__android_log_print(ANDROID_LOG_WARN, "P2P", "MoloClient::env is  = %u", this->getEnv()); 
	
	__android_log_print(ANDROID_LOG_WARN, "P2P", "MoloClient::JVM is  = %u", this->getJVM());	 
	    
		JNIEnv* env;                
		this->getJVM()->AttachCurrentThread(&env, NULL); 
	
		jclass cls = env->GetObjectClass(this->getCallback());
	    jmethodID jmid = env->GetMethodID(cls, "output", "(Ljava/lang/String;)V");
	    jstring info = env->NewStringUTF(word.substr(0, len).c_str());
	    env->CallVoidMethod(this->getCallback(), jmid,info);
	    //env->ReleaseStringUTFChars(info,env->GetStringUTFChars(info, FALSE));    
	   
	
	
		//JNIEnv* env;	      
		//this->getJVM()->AttachCurrentThread(&env, NULL);  
		// 		  
		//jclass clz = env->FindClass("com/molo/app/NativeMethod");
		// 					
		// 		jmethodID ctor = env->GetMethodID(clz, "<init>", "()V");
		// 		jobject obj = env->NewObject(clz, ctor);
		// 		jmethodID mid = env->GetMethodID(clz, "output", "(Ljava/lang/String;)V");  
		// 		jstring info = env->NewStringUTF("i am a callback!");
		// 	    env->CallObjectMethod(obj, mid,info);         
}

void MoloClient::OnCandidate(cricket::TransportChannelImpl* ch,
		const cricket::Candidate& c) {   
	LOGD("MoloClient::OnCandidate");
	LOG(LS_WARNING) <<"remote is :"<<this->getRemoteUser();
	buzz::XmlElement* cand_elem = new buzz::XmlElement(QN_GINGLE_P2P_CANDIDATE);
	WriteError error;
	WriteCandidate(c, cand_elem, &error);
	ready_candidates_.push_back(c);
	buzz::XmlElement* stanza = new buzz::XmlElement(buzz::QN_MESSAGE);
	if (this->getRemoteUser().length() > 0) {
	  const std::string remoteUser = this->getRemoteUser();
		stanza->AddAttr(buzz::QN_TO, remoteUser);
	} else {
	  LOG(LS_INFO) << "error,get name failed!" << this->to;
		stanza->AddAttr(buzz::QN_TO, "gc1@molohui.com");
	}
	stanza->AddAttr(buzz::QN_ID, talk_base::CreateRandomString(16));
	stanza->AddAttr(buzz::QN_TYPE, "chat");
	stanza->AddElement(cand_elem);
	xmpp_client_->SendStanza(stanza);
	delete stanza;
}

bool MoloClient::WriteCandidate(const cricket::Candidate& candidate,
		buzz::XmlElement* elem, cricket::WriteError* error) {
	elem->SetAttr(buzz::QN_NAME, candidate.name());
	elem->SetAttr(QN_ADDRESS, candidate.address().IPAsString());
	elem->SetAttr(QN_PORT, candidate.address().PortAsString());
	elem->SetAttr(QN_PREFERENCE, candidate.preference_str());
	elem->SetAttr(QN_USERNAME, candidate.username());
	elem->SetAttr(QN_PROTOCOL, candidate.protocol());
	elem->SetAttr(QN_GENERATION, candidate.generation_str());
	if (candidate.password().size() > 0)
		elem->SetAttr(QN_PASSWORD, candidate.password());
	if (candidate.type().size() > 0)
		elem->SetAttr(buzz::QN_TYPE, candidate.type());
	if (candidate.network_name().size() > 0)
		elem->SetAttr(QN_NETWORK, candidate.network_name());
	return true;
}

void MoloClient::HandleChat(const buzz::XmlElement* elem) {
	const buzz::XmlElement* chatElement = elem->FirstElement();
	const std::string chat = chatElement->BodyText();
	LOG(LS_INFO) << "HandleChat in MoloClient, chat is : " << chat;
}

void MoloClient::HandleCandidate(const buzz::XmlElement* stanza) {
  //const buzz::XmlElement* elem = stanza->FirstNamed(QN_GINGLE_P2P_CANDIDATE);
  //const buzz::XmlElement* chatElement = stanza->FirstNamed(buzz::QN_MESSAGE);
  //const std::string& from = chatElement->Attr(buzz::QN_FROM);
  //	LOG(LS_WARNING) << "MoloClient::HandleCandidate";
  //	LOG(LS_WARNING) << "Stanza:"<<stanza->Str();
  //	LOG(LS_WARNING) << "Candidate:"<<elem->Str();
  //	LOG(LS_WARNING) << "from:"<<from;
LOG(LS_WARNING) << "Stanza:"<<stanza->Str();
	const buzz::XmlElement* elem = stanza->FirstNamed(QN_GINGLE_P2P_CANDIDATE);
	const buzz::XmlElement* chatElement = stanza->FirstNamed(buzz::QN_MESSAGE);
	if (elem) {
		LOG(LS_WARNING) << "Candidate:" << elem->Str();
	}
	const std::string& from = stanza->Attr(buzz::QN_FROM);
	LOG(LS_WARNING) << "from:" << from;
	int pos = from.find("/");
	const std::string& remoteuser = from.substr(0,pos);
	LOG(LS_WARNING) << "remote_user:" << remoteuser;
	if (remoteuser.length() > 0) {
	  this->setRemoteUser(remoteuser);	
	}

	LOG(LS_WARNING) << "remote_user:" << remoteuser;

	LOG(LS_WARNING) << "MoloClient::HandleCandidate";

	cricket::Candidate candidate;
	ParseError error;
	talk_base::SocketAddress address;
	if (!ParseAddress(elem, QN_ADDRESS, QN_PORT, &address, &error)) {
		LOG(LS_INFO) << elem->Attr(QN_ADDRESS);
		LOG(LS_INFO) << "MoloClient::HandleCandidate,ERROR!";
		return;
	}
	candidate.set_name(elem->Attr(buzz::QN_NAME));
	candidate.set_address(address);
	candidate.set_username(elem->Attr(QN_USERNAME));
	candidate.set_preference_str(elem->Attr(QN_PREFERENCE));
	candidate.set_protocol(elem->Attr(QN_PROTOCOL));
	candidate.set_generation_str(elem->Attr(QN_GENERATION));
	if (elem->HasAttr(QN_PASSWORD))
		candidate.set_password(elem->Attr(QN_PASSWORD));
	if (elem->HasAttr(buzz::QN_TYPE))
		candidate.set_type(elem->Attr(buzz::QN_TYPE));
	if (elem->HasAttr(QN_NETWORK))
		candidate.set_network_name(elem->Attr(QN_NETWORK));
	remote_candidates_.push_back(candidate);

	if (channel != NULL) {  
		LOGD("RememberRemoteCandidate");
		channel->RememberRemoteCandidate(candidate, NULL);
		channel_is_not_work_ = false;
	} else //no need to confirm,just accept immediately.
	{    
		LOGD("Not RememberRemoteCandidate");  
		worker_thread_->PostDelayed(1000, this, MSG_CREATECHANNEL);
	}
//	channel->OnCandidate(candidate);
}

bool MoloClient::ParseCandidate(const buzz::XmlElement* elem,
		cricket::Candidate* candidate, cricket::ParseError* error) {
	if (!elem->HasAttr(buzz::QN_NAME) ||
	      !elem->HasAttr(QN_ADDRESS) ||
	      !elem->HasAttr(QN_PORT) ||
	      !elem->HasAttr(QN_USERNAME) ||
	      !elem->HasAttr(QN_PREFERENCE) ||
	      !elem->HasAttr(QN_PROTOCOL) ||
	      !elem->HasAttr(QN_GENERATION)) {
	    return BadParse("candidate missing required attribute", error);
	  }

	  talk_base::SocketAddress address;
	  if (!ParseAddress(elem, QN_ADDRESS, QN_PORT, &address, error))
	    return false;

	  candidate->set_name(elem->Attr(buzz::QN_NAME));
	  candidate->set_address(address);
	  candidate->set_username(elem->Attr(QN_USERNAME));
	  candidate->set_preference_str(elem->Attr(QN_PREFERENCE));
	candidate->set_protocol(elem->Attr(QN_PROTOCOL));
	candidate->set_generation_str(elem->Attr(QN_GENERATION));
	if (elem->HasAttr(QN_PASSWORD))
		candidate->set_password(elem->Attr(QN_PASSWORD));
	if (elem->HasAttr(buzz::QN_TYPE))
		candidate->set_type(elem->Attr(buzz::QN_TYPE));
	if (elem->HasAttr(QN_NETWORK))
		candidate->set_network_name(elem->Attr(QN_NETWORK));

	return true;
}

bool MoloClient::ParseAddress(const buzz::XmlElement* elem,
		const buzz::QName& address_name, const buzz::QName& port_name,
		talk_base::SocketAddress* address, ParseError* error) {
	if (!elem->HasAttr(address_name)) {
		LOG(LS_INFO)<<"address_name not have";
		return BadParse("address does not have " + address_name.LocalPart(),
				error);
	}

	if (!elem->HasAttr(port_name)) {
		LOG(LS_INFO)<<"port_name not have";
		return BadParse("address does not have " + port_name.LocalPart(), error);
	}
	address->SetIP(elem->Attr(address_name));
	std::istringstream ist(elem->Attr(port_name));
	int port = 0;
	ist >> port;
	address->SetPort(port);

	return true;
}

void MoloClient::SetAvailable(const buzz::Jid& jid, buzz::Status* status) {
	status->set_jid(jid);
	status->set_available(true);
	status->set_show(buzz::Status::SHOW_ONLINE);
}

void MoloClient::OnRequestSignaling() {
	session_manager_->OnSignalingReady();
}

void MoloClient::OnSessionCreate(cricket::Session* session, bool initiate) {
	session->set_allow_local_ips(true);
	session->set_current_protocol(cricket::PROTOCOL_HYBRID);
}

void MoloClient::MakeCallTo(const std::string& name) {
	//buzz::Jid callto_jid(name);
	//if (!call_) {
	   // call_ = molo_client_->CreateCall();
	   // session_ = call_->InitiateSession(callto_jid);
	//}
}

void MoloClient::SendChat(const std::string& to, const std::string msg) {
	buzz::XmlElement* stanza = new buzz::XmlElement(buzz::QN_MESSAGE);
	stanza->AddAttr(buzz::QN_TO, to);
	stanza->AddAttr(buzz::QN_ID, talk_base::CreateRandomString(16));
	stanza->AddAttr(buzz::QN_TYPE, "chat");
	buzz::XmlElement* body = new buzz::XmlElement(buzz::QN_BODY);
	body->SetBodyText(msg);
	stanza->AddElement(body);
	xmpp_client_->SendStanza(stanza);

	delete stanza;
}


const std::string MoloClient::strerror(buzz::XmppEngine::Error err) {
	switch (err) {
	case buzz::XmppEngine::ERROR_NONE:
		return "";
	case buzz::XmppEngine::ERROR_XML:
		return "Malformed XML or encoding error";
	case buzz::XmppEngine::ERROR_STREAM:
		return "XMPP stream error";
	case buzz::XmppEngine::ERROR_VERSION:
		return "XMPP version error";
	case buzz::XmppEngine::ERROR_UNAUTHORIZED:
		return "User is not authorized (Check your username and password)";
	case buzz::XmppEngine::ERROR_TLS:
		return "TLS could not be negotiated";
	case buzz::XmppEngine::ERROR_AUTH:
		return "Authentication could not be negotiated";
	case buzz::XmppEngine::ERROR_BIND:
		return "Resource or session binding could not be negotiated";
	case buzz::XmppEngine::ERROR_CONNECTION_CLOSED:
		return "Connection closed by output handler.";
	case buzz::XmppEngine::ERROR_DOCUMENT_CLOSED:
		return "Closed by </stream:stream>";
	case buzz::XmppEngine::ERROR_SOCKET:
		return "Socket error";
	default:
		return "Unknown error";
	}
}
