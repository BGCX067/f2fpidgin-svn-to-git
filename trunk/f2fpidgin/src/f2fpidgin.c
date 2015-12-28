#ifndef PURPLE_PLUGINS
#define PURPLE_PLUGINS
#endif

#include <glib.h>
#include <string.h>
#include <math.h>
#include <stdlib.h>
#include <python2.5/Python.h>

#include <notify.h>
#include <plugin.h>
#include <version.h>
#include <request.h>
#include <debug.h>
#include <conversation.h>
#include <signal.h>
#include "f2fcore.h"
#include "f2fticketrequest.h"
#include "f2fgrouplist.c"
#include "b64/b64.h"

#define PLUGIN_AUTHOR "Maido Käära <vermon@gmail.com>"
#define PLUGIN_VERSION "0.1"
#define PLUGIN_NAME "F2F Network Plugin"
#define PLUGIN_ID "core-vermon-f2fplugin"
#define SHOW_CORE_MESSAGES FALSE

/** Used to keep friends name, protocol and whos friend is she*/
typedef struct Friend_
{
	char* name;
	char* me;
	char* protocol;
} Friend;

/** Used to keep a message and a friend who sent it*/
typedef struct Message_
{
	Friend* friend;
	char* message;
} Message;

PurplePlugin* pluginHandle = NULL;
F2FPeer* f2fPeer = NULL;
PurpleRequestField* chatChoice = NULL;
GList* f2fChats = NULL;
GList* f2fGroups = NULL;
GList* f2fPeers = NULL;
GAsyncQueue* messages = NULL;

/**Helper Methods*/

static void
printPeerList()
{
	int i;
	for(i = 0;i<f2fPeerListGetSize();i++)
	{
		F2FPeer* peer = f2fPeerListGetPeer(i);
		purple_debug_misc(PLUGIN_ID,"Peerid: %d,%d localpeerid: %d\n",f2fPeerGetUIDHi(peer),f2fPeerGetUIDLo(peer),f2fPeerGetLocalPeerId(peer));
	}
}

static void
printGroupList()
{
	int i;
	for(i = 0;i < groupListSize ;i++)
	{
		F2FGroup group = groupList[i];
		purple_debug_misc(PLUGIN_ID,"Group: %s\n",group.name);
	}
}

/**
 * Iterate over groups in core
 * Print group name
 * Iterate over peers in group
 * Print name
 */
static void printF2FGroupsAndPeersWithin(){
	purple_debug_misc(PLUGIN_ID,"Local F2FPeer [%d,%d]\n",f2fPeerGetUIDHi(f2fPeer), f2fPeerGetUIDLo(f2fPeer));
	int i;
	for (i = 0; i < groupListSize; i++){
		F2FGroup f2f_group = groupList[i];
		int n = f2f_group.listSize;
		purple_debug_misc(PLUGIN_ID,"Group: [%s] Peer count [%d]\n",f2f_group.name, n);

		int j;
		for (j =0; j < n; j++){
			F2FPeer* peer = f2fPeerListGetPeer(j);
			purple_debug_misc(PLUGIN_ID,"Peerid: [%d,%d] localpeerid: [%d]\n",f2fPeerGetUIDHi(peer),f2fPeerGetUIDLo(peer),f2fPeerGetLocalPeerId(peer));
		}
	}
}


static void
printGroupsAndPeers()
{
	int i;
	for(i = 0;i<g_list_length(f2fGroups);i++)
	{
		F2FGroup* group = g_list_nth_data(f2fGroups,i);
		purple_debug_misc(PLUGIN_ID,"Group name: %s\n",group->name);
		int j;
		for(j = 0; j < f2fGroupGetPeerListSize( group );j++)
		{
			F2FPeer* peer = f2fGroupGetPeerFromList( group, j );
			purple_debug_misc(PLUGIN_ID,"Peerid: %d,%d localpeerid: %d\n",f2fPeerGetUIDHi(peer),f2fPeerGetUIDLo(peer),f2fPeerGetLocalPeerId(peer));
		}

	}
}

/* Escapes F2F Tag for IM sending*/
static char*
escapeF2FMessage(const char* message)
{
	char* escaped = g_markup_escape_text(F2FMessageMark,-1);
	char* result = calloc(strlen(escaped)+strlen(message)-F2FMessageMarkLength+1,sizeof(char));
	strcpy(result,escaped);
	free(escaped);
	strcat(result,message+F2FMessageMarkLength);
	return result;
}

/** Encodes message so core gets it */
static void
encodeMessage( char** message )
{
	purple_debug_info(PLUGIN_ID,"Initial message %s\n",*message);
	F2FSize currentsize, newsize;
	currentsize = F2FMessageMarkLength;
	char* encoded = calloc(((strlen(*message)+3/3)*4)+currentsize+1,sizeof(char));
	strcpy( encoded, F2FMessageMark );
	newsize = b64encode( *message, encoded+currentsize, strlen(*message), (strlen(*message)+3/3)*4);
	free(*message);
	if(newsize == 0) {purple_debug_info(PLUGIN_ID,"Something is wrong, Encoding failed!");return;}
	char* escaped = escapeF2FMessage(encoded);
	purple_debug_info(PLUGIN_ID,"Encoded and escaped: %s\n",escaped);
	*message = calloc(strlen(escaped)+1,sizeof(char));
	strcpy(*message,escaped);
	free(encoded);
	free(escaped);
}

/** Compares friends so they can be searched from GList*/
static int
compareFriend(const Friend* friend1, const Friend* friend2){
	int i = strcmp(friend1->name,friend2->name);
	if(i==0)
	{
		return strcmp(friend1->protocol,friend2->protocol);
	}
	else
	{
		return i;
	}
}

/**Gets Friend from list or adds new and returns localID*/
static int
getOrAddFriend(Friend* friend)
{
	GList* f2fPeer = g_list_find_custom(f2fPeers,friend,(GCompareFunc)compareFriend);
			if(f2fPeer == NULL)
			{
				f2fPeers = g_list_append(f2fPeers,friend);
				purple_debug_info(PLUGIN_ID,"Added Friend name:%s and protocol:%s\n",friend->name,friend->protocol);
				f2fPeer = g_list_find_custom(f2fPeers,friend,(GCompareFunc)compareFriend);
			}
	return g_list_position(f2fPeers,f2fPeer);
}

/** Helper for creating a new friend*/
static Friend*
createNewFriend(PurpleAccount* account, const char* name)
{
	Friend* friend = malloc(sizeof(Friend));
	friend->name = malloc(strlen(name)+1);
	strcpy(friend->name,name);
	friend->protocol = malloc(strlen(account->protocol_id)+1);
	strcpy(friend->protocol,account->protocol_id);
	friend->me = malloc(strlen(account->username)+1);
	strcpy(friend->me,account->username);
	return friend;
}

/** Generates request fields for choosing a conversation to create a group*/
static PurpleRequestFields*
getChatFields()
{
		PurpleRequestFields* fields = purple_request_fields_new();
	 	PurpleRequestFieldGroup* group = purple_request_field_group_new (NULL);
	 	chatChoice = purple_request_field_choice_new ("choice", "Choose", 0);
	 	GList* convs = purple_get_conversations();
	 	int i;
	 	for(i = 0;i<g_list_length(convs);i++)
	 	{
	 		PurpleConversation* conv = g_list_nth_data(convs,i);
	 		purple_request_field_choice_add (chatChoice, conv->name);
	 	}
	 	purple_request_field_group_add_field (group, chatChoice);
	 	purple_request_fields_add_group (fields, group);

	 	return fields;
}

/**Callback functions*/

/** Submits a job to a group*/
static void
chooseJobCb(F2FGroup* group, const char* filename)
{
	purple_debug_info(PLUGIN_ID,"Job Submission Selected filename [%s]\n",filename);
	purple_debug_info(PLUGIN_ID,"Job Submission Selected group [%s]\n",group->name);

	//TODO: Cannot call F2FCore here, not threadsafe!
	F2FError status = f2fGroupSubmitJob(group,filename);
	purple_debug_info(PLUGIN_ID,"Submitting job. Got status %d while opening %s\n",status,filename);
}

static void submitJobOkBt(PurplePluginAction* action){
	int choice = purple_request_field_choice_get_value (chatChoice);
	purple_debug_info(PLUGIN_ID,"User chose %d\n",choice);
	//PurpleConversation* conv = g_list_nth_data(f2fChats,choice);
	F2FGroup* group = NULL;
	//const char* convName = purple_conversation_get_name(conv);
	//int j;
	//for(j = 0; j < g_list_length(f2fGroups);j++)
	//{
		F2FGroup* grp = g_list_nth_data(f2fGroups,choice);
	//	if(strcmp(convName,grp->name) == 0)
	//	{
			group = grp;
	//	}
	//}
	purple_debug_info(PLUGIN_ID,"Selected group [%s]\n",group->name);
	purple_request_file(
			pluginHandle,
			"choose Job file",
			NULL,
			FALSE,
			G_CALLBACK(chooseJobCb),
			NULL,
			NULL,
			NULL,
			NULL,
			group);
}

/** Creates a new group or submits a job to excisting group*/
static void
submitJobOkCb (PurplePluginAction* action)
{
	int choice = purple_request_field_choice_get_value (chatChoice);
	purple_debug_info(PLUGIN_ID,"User chose %d\n",choice);
	F2FGroup* group = NULL;
	PurpleConversation* conv = NULL;
	conv = g_list_nth_data(f2fChats,choice);
	PurpleAccount* account = purple_conversation_get_account(conv);
	PurpleConversationType type = purple_conversation_get_type(conv);
	const char* convName = purple_conversation_get_name(conv);
	int j;
	for(j = 0; j < g_list_length(f2fGroups);j++)
	{
		F2FGroup* grp = g_list_nth_data(f2fGroups,j);
		if(!strcmp(convName,grp->name))
		{
			group = grp;
			purple_debug_info(PLUGIN_ID,"This group already exists!\n");
		}
	}
	if(group == NULL)
	{
		if(type == PURPLE_CONV_TYPE_CHAT)
		{
			purple_debug_info(PLUGIN_ID,"Conversation type is chat\n");
			PurpleConvChat* chat = PURPLE_CONV_CHAT(conv);
			group = f2fCreateGroup( conv->name );
			purple_debug_info(PLUGIN_ID,"Created a new group with name: %s\n",group->name);
			f2fGroups = g_list_append(f2fGroups,group);
			GList* users = purple_conv_chat_get_users(chat);
			int i;
			for(i = 0;i<g_list_length(users);i++)
			{
				PurpleConvChatBuddy* buddy = g_list_nth_data(users,i);
				char* name = NULL;
				name = buddy->name;
				purple_debug_info(PLUGIN_ID,"Buddy name %s\n",name);
				Friend* friend = createNewFriend(account,name);
				int localId = getOrAddFriend(friend);
				//TODO: Cannot call F2FCore here, not threadsafe!
				f2fGroupRegisterPeer(
					group,
					localId,
					name,
					"Hello",
					NULL );
			}
		}
		else if(type == PURPLE_CONV_TYPE_IM)
		{
			group = f2fCreateGroup( conv->name );
			purple_debug_info(PLUGIN_ID,"Created a new group with name: %s\n",group->name);
			f2fGroups = g_list_append(f2fGroups,group);
			Friend* friend = createNewFriend(account,convName);
			int localId = getOrAddFriend(friend);
			char* name = malloc(strlen(convName)+1);
			strcpy(name,convName);
			//TODO: Cannot call F2FCore here, not threadsafe!
			f2fGroupRegisterPeer(
				group,
				localId,
				name,
				"Hello",
				NULL );
		}
	}
	purple_request_file(
		pluginHandle,
		"choose Job file",
		NULL,
		FALSE,
		G_CALLBACK(chooseJobCb),
		NULL,
		NULL,
		NULL,
		NULL,
		group);
}

static void initF2FGroup(PurpleConversation* conv){
	PurpleAccount* account = purple_conversation_get_account(conv);
	PurpleConversationType type = purple_conversation_get_type(conv);
	const char* convName = purple_conversation_get_name(conv);

	if(type == PURPLE_CONV_TYPE_CHAT){
		//TODO:
	} else if (type == PURPLE_CONV_TYPE_IM){

		// create group
		F2FGroup* f2fGroup = f2fCreateGroup( conv->name );
		purple_debug_info(PLUGIN_ID,"Created a new group with name: %s\n",f2fGroup->name);
		f2fGroups = g_list_append(f2fGroups,f2fGroup);
		// add friend
		Friend* friend = createNewFriend(account, convName);
		purple_debug_info(PLUGIN_ID,"Got Friend [%s] from conversation [%s]", friend->name, convName);
		int localId = getOrAddFriend(friend);	// this should be +1
		purple_debug_info(PLUGIN_ID,"Friend [%s] localId [%d]", friend->name, localId);
		char* identificator = malloc(strlen(friend->name)+1);
		strcpy(identificator,friend->name);
		f2fGroupRegisterPeer(f2fGroup,localId,identificator,"Helo",NULL);
	}
}

static void initGroupBt(PurplePluginAction* action){
	//for testing initGroup
	int choice = purple_request_field_choice_get_value (chatChoice);
	purple_debug_info(PLUGIN_ID,"User chose %d\n",choice);
	PurpleConversation* conv = g_list_nth_data(f2fChats,choice);
	initF2FGroup(conv);
}

static void
pluginActionSubmitJobCb (PurplePluginAction* action)
{
	/*
	purple_request_fields(
		pluginHandle,
		"Choose F2F Conversation",
		"Choose Conversation",
		NULL,
		getChatFields(),
		"OK",
		G_CALLBACK(submitJobOkCb),
		"Cancel",
		NULL,
		NULL,
		NULL,
		NULL,
		NULL
	);
	*/
	purple_request_fields(
			pluginHandle,
			"Choose F2F Conversation",
			"Choose Conversation",
			NULL,
			getChatFields(),
			"OK",
			G_CALLBACK(submitJobOkBt),
			"Cancel",
			NULL,
			NULL,
			NULL,
			NULL,
			NULL
		);
}

/** Lets you add a friend to a computational group*/
static void
pluginActionAddFriendCb (PurplePluginAction* action)
{
	//TODO: Add a friend to a group
	purple_request_fields(
				pluginHandle,
				"Choose F2F Conversation",
				"Choose Conversation",
				NULL,
				getChatFields(),
				"OK",
				G_CALLBACK(initGroupBt),
				"Cancel",
				NULL,
				NULL,
				NULL,
				NULL,
				NULL
			);

}

/**
*  Called when conversation is created.
*  Adds an identifier to chats name
*/
static void
conversationCreatedCb(PurpleConversation* conv)
{
	PurpleConversationType type = purple_conversation_get_type(conv);
	if(type == PURPLE_CONV_TYPE_CHAT)
	{
		srand (time (0));
		int random = rand();
		int* id = malloc(sizeof(int));
		*id = random;
		char* sId = calloc(log(random)+5,sizeof(char));
		sprintf(sId," - %d",random);
		char* name = calloc(strlen(conv->name)+strlen(sId)+1,sizeof(char));
		strcat(name,conv->name);
		strcat(name,sId);
		free(sId);
		purple_conversation_set_name(conv,name);
		purple_conversation_set_data(conv,"f2fid",id);
	}
	f2fChats = g_list_append(f2fChats,conv);

	//
	//initF2FGroup(conv);
}

/** If conversation is deleted, then removes it from the list and unregisters local peer*/
static void
conversationDeletedCb(PurpleConversation* conv)
{
	purple_debug_info(PLUGIN_ID,"Removing conversation\n");
	int i;
	for(i = 0; i < g_list_length(f2fGroups);i++)
	{
		F2FGroup* grp = g_list_nth_data(f2fGroups,i);
		if(!strcmp(grp->name,conv->name))
		{
			//TODO: Cannot call F2FCore here, not threadsafe!
			f2fGroupUnregisterPeer(grp, f2fPeer);
			break;
		}
	}
	f2fChats = g_list_remove(f2fChats,conv);
}

static void
buddyJoinedCb(PurpleConversation* conv, const char* name, PurpleConvChatBuddyFlags flags, gboolean new_arrival)
{
	purple_debug_info(PLUGIN_ID,"User is joining chat\n");
	int i;
	for(i = 0;i<g_list_length(f2fGroups);i++)
	{
		F2FGroup* group = g_list_nth_data(f2fGroups,i);
		if(!strcmp(group->name,conv->name))
		{
			PurpleAccount* account = purple_conversation_get_account(conv);
			Friend* friend = createNewFriend(account,name);
			int localId = getOrAddFriend(friend);
			char* peerName = malloc(strlen(name)+1);
			strcpy(peerName,name);
			//TODO: Cannot call F2FCore here, not threadsafe!
			f2fGroupRegisterPeer(
				group,
				localId,
				peerName,
				"Hello",
				NULL );
		}
	}

}

/** Called when a friend is leaving chat*/
static void
buddyLeftCb(PurpleConversation* conv, const char* name, const char* reason)
{
	purple_debug_info(PLUGIN_ID,"User is leaving chat\n");
	int i;
	for(i = 0;i<g_list_length(f2fGroups);i++)
	{
		F2FGroup* group = g_list_nth_data(f2fGroups,i);
		if(!strcmp(group->name,conv->name))
		{
			PurpleAccount* account = purple_conversation_get_account(conv);
			Friend* friend = createNewFriend(account, name);
			int localId = getOrAddFriend(friend);
			//TODO: Cannot call F2FCore here, not threadsafe!
			F2FPeer* peer = f2fPeerListGetPeer(localId);
			f2fGroupUnregisterPeer( group, peer );
		}
	}
}

/* Called when you send an IM. Used to encode messages so core gets them*/
static void
messageSendingIMCb(PurpleAccount* account, const char* receiver, char** message)
{
	int i;
	for(i = 0; i < g_list_length(f2fGroups);i++)
	{
		F2FGroup* group = NULL;
		group = g_list_nth_data(f2fGroups,i);
		if(!strcmp(receiver,group->name))
		{
			encodeMessage(message);
		}
	}
}

/* Called when you send a message in Chat. Used to encode messages so core gets them*/
static void
messageSendingChatCb(PurpleAccount* account, char** message, int id)
{
	PurpleConversation* conv = NULL;
	int i;
	for(i = 0 ; i < g_list_length(f2fChats);i++)
	{
		PurpleConvChat* ch = PURPLE_CONV_CHAT(g_list_nth_data(f2fChats,i));
		if(id == purple_conv_chat_get_id(ch)) conv = purple_conv_chat_get_conversation(ch);
	}

	for(i = 0 ; i < g_list_length(f2fGroups);i++)
	{
		F2FGroup* group = g_list_nth_data(f2fGroups,i);
		if(!strcmp(purple_conversation_get_name(conv),group->name))
		{
			encodeMessage(message);
		}
	}
}

/* Called when someone writes a message to you*/
static gboolean
messageReceivingCb(PurpleAccount* account, char** sender,char** message, PurpleConversation* conv,PurpleMessageFlags* flags)
{
	/*
	purple_debug_info(PLUGIN_ID,"Receiving message from %s : %s\n Protocol: %s\n",*sender,*message,purple_account_get_protocol_name(account));
	Friend* friend = createNewFriend(account,*sender);
	if( memcmp( purple_markup_strip_html(*message), F2FMessageMark, F2FMessageMarkLength ) ) return FALSE;
	getOrAddFriend(friend);
	Message* msg = malloc(sizeof(Message));
	msg->friend = friend;
	msg->message = purple_markup_strip_html(*message);
	purple_debug_info(PLUGIN_ID,"Received message: \n%s\n",purple_markup_strip_html(*message));
	g_async_queue_push(messages,msg);
	*/

	char* stripped = purple_markup_strip_html(*message);
	if (strncmp(stripped, F2FMessageMark, F2FMessageMarkLength) == 0){
		purple_debug_info(PLUGIN_ID,"Accepting f2f message\n%s\n",stripped);
		// get friend
		Friend* friend = createNewFriend(account,*sender);
		// check if exist
		int localId = getOrAddFriend(friend);

		// create message
		Message* msg = malloc(sizeof(Message));
		msg->friend = friend;
		msg->message = stripped;

		purple_debug_info(PLUGIN_ID,"Message sender [%s]\n", friend->name);
		purple_debug_info(PLUGIN_ID,"Forwarding to core queue\n");

		g_async_queue_push(messages,msg);


	} else {
		purple_debug_info(PLUGIN_ID,"Discarding malformed message\n%s\n",stripped);
	}

	if(!SHOW_CORE_MESSAGES) return TRUE;
	return FALSE;
}

/** Sends messages from F2FCore to another peer*/
static void
sendToPurple(F2FAdapterReceiveMessage* message)
{
	F2FGroup* group = NULL;
	group = f2fMessageGetGroup(message);
	char* msg = f2fMessageGetContentPtr(message);
	if(group == NULL)
	{
		purple_debug_info(PLUGIN_ID,"Group is null\n");
		F2FWord32 localId = -1;
		F2FPeer* peer = NULL;
		peer = f2fMessageGetSourcePeer(message);
		if (peer != NULL)
		{
			purple_debug_info(PLUGIN_ID,"Peerid: %d,%d localpeerid: %d\n",f2fPeerGetUIDHi(peer),f2fPeerGetUIDLo(peer),f2fPeerGetLocalPeerId(peer));
		}
		localId = f2fMessageGetNextLocalPeerID(message);
		while(localId != -1)
		{
			purple_debug_info(PLUGIN_ID,"Local peer id is %d\n",localId);
			Friend* friend = NULL;
			friend = g_list_nth_data(f2fPeers,localId);
			purple_debug_info(PLUGIN_ID,"Friend %s and %s!\n",friend->name,friend->protocol);
			PurpleAccount* account = purple_accounts_find (friend->me, friend->protocol);
			serv_send_im(purple_account_get_connection(account),friend->name,escapeF2FMessage(msg),PURPLE_MESSAGE_SEND);
			localId = f2fMessageGetNextLocalPeerID(message);
		}
		purple_debug_info(PLUGIN_ID,"Message sent: \n%s\n",msg);
	}
	else
	{
		purple_debug_info(PLUGIN_ID,"Group is NOT null\n");
		int i;
		for(i = 0;i<g_list_length(f2fChats);i++)
		{
			PurpleConversation* conv = g_list_nth_data(f2fChats,i);
			if(strcmp(conv->name,group->name))
			{
				purple_debug_info(PLUGIN_ID,"Found corresponding conversation!\n");
				PurpleConversationType type = purple_conversation_get_type (conv);
				if(type == PURPLE_CONV_TYPE_CHAT)
				{
					purple_conv_chat_send(PURPLE_CONV_CHAT(conv),escapeF2FMessage(msg));
				}
				else if(type == PURPLE_CONV_TYPE_IM)
				{
					purple_conv_im_send (PURPLE_CONV_IM(conv),escapeF2FMessage(msg));
				}
			}
		}
	}
}

/** Takes received messages from a GList and sends them to the core*/
static void
evaluateReceivedMessages()
{
	g_async_queue_ref(messages);
	Message* message = NULL;
	message = g_async_queue_try_pop(messages);
	while(message != NULL)
	{
		//TODO: ??
		int localId = getOrAddFriend(message->friend);
		//

		F2FError status = f2fForward(
							localId,
							message->friend->name,
							message->message,
							strlen(message->message) );

		if(status == F2FErrOK)
		{
			purple_debug_info(PLUGIN_ID,"Sent received message to core!\n");
		}
		else
		{
			purple_debug_info(PLUGIN_ID,"Core didn't want message!\n");
		}
		message = g_async_queue_try_pop(messages);
	}
	g_async_queue_unref(messages);
}
/* Gets messages from F2F Core and evaluates them*/
static gboolean
evaluateMessagesCb()
{
	//printGroupList();
	//printPeerList();
	//printGroupsAndPeers();

	printF2FGroupsAndPeersWithin();

	evaluateReceivedMessages();
	while(f2fMessageAvailable()){
		F2FAdapterReceiveMessage* message = f2fReceiveMessage();
		purple_debug_info(PLUGIN_ID,"Core wants to send!\n");
		if(message != NULL) {

			//debug
			if (message->sourcePeer != NULL){
				purple_debug_info(PLUGIN_ID,"Send from [%s]\n",message->sourcePeer->identifier);
			} else {
				purple_debug_info(PLUGIN_ID,"Send from [null]\n");
			}

			if (message->destPeer != NULL){
				purple_debug_info(PLUGIN_ID,"Send to [%s]\n",message->destPeer->identifier);
			} else {
				purple_debug_info(PLUGIN_ID,"Send to [null]\n");
			}
			if (message->group != NULL){
				purple_debug_info(PLUGIN_ID,"Group [%s]\n",message->group->name);
			} else {
				purple_debug_info(PLUGIN_ID,"Group[ [null]\n");
			}
			// debug

			if(f2fMessageIsForward(message)) {
				purple_debug_info(PLUGIN_ID,"Message needs to be forwarded\n");
				sendToPurple(message);
				f2fMessageRelease(message);
			}
			else if(f2fMessageIsRaw(message)) {
				purple_debug_info(PLUGIN_ID,"Message needs to be saved\n");
				char* content = f2fMessageGetContentPtr(message);
				F2FGroup* group = f2fMessageGetGroup(message);
				F2FPeer* initiator = f2fMessageGetSourcePeer(message);
				//f2fPeerSendRaw(group,peer,serialdata,strlen(serialdata));
				f2fMessageRelease(message);
			}
			else if(f2fMessageIsText(message)){
				purple_debug_info(PLUGIN_ID,"Message is text\n");
				purple_debug_info(PLUGIN_ID,"Got message from Core: %s",f2fMessageGetContentPtr(message));
				f2fMessageRelease(message);
			}
			else if(f2fMessageIsJob(message)) {
				purple_debug_info(PLUGIN_ID,"Message is a job and needs to be run!\n");
				F2FGroup* group = f2fMessageGetGroup(message);
				F2FPeer* initiator = f2fMessageGetSourcePeer(message);
				int length = strlen(message->buffer);
				char* job = malloc(length);
				int* maxSize = malloc(log(length)+1);
				memcpy(maxSize,&length,log(length)+1);
				f2fMessageGetJob(message, job, maxSize );
				f2fMessageRelease(message);
				//TODO: Running a job
				PyObject* jobObj = Py_CompileString(job, "<f2f job>", Py_file_input);
				purple_debug_info(PLUGIN_ID,"Running job!\n");
				//PyEval_EvalCode(jobObj,Py_BuildValue("{}"),Py_BuildValue("{}"));

			}
		}
		f2fTicketRequestGrant();
	}
	return TRUE;
}

/* List of plugin actions. Shown in the menu*/
static GList*
pluginActions (PurplePlugin* plugin, gpointer context)
{
	GList* list = NULL;
	PurplePluginAction* action = NULL;

	action = purple_plugin_action_new("Submit a Job", pluginActionSubmitJobCb);
	list = g_list_append (list,action);

	action = purple_plugin_action_new("Add friend to a group", pluginActionAddFriendCb);
	list = g_list_append (list,action);

	return list;
}

/** Called when plugin is loaded*/
static gboolean
pluginLoad (PurplePlugin*  plugin)
{

	pluginHandle = plugin; /* assign this here so we have a valid handle later*/

	/** Connecting callback functions for receiving messages*/
	purple_signal_connect(purple_conversations_get_handle(),"receiving-im-msg",pluginHandle,PURPLE_CALLBACK(messageReceivingCb),NULL);
	purple_signal_connect(purple_conversations_get_handle(),"receiving-chat-msg",pluginHandle,PURPLE_CALLBACK(messageReceivingCb),NULL);

	/** Connecting callback functions for sending messages*/
	purple_signal_connect(purple_conversations_get_handle(),"sending-im-msg",pluginHandle,PURPLE_CALLBACK(messageSendingIMCb),NULL);
	purple_signal_connect(purple_conversations_get_handle(),"sending-chat-msg",pluginHandle,PURPLE_CALLBACK(messageSendingChatCb),NULL);

	/** Connecting callback functions for conversation creating and destroying*/
	purple_signal_connect(purple_conversations_get_handle(),"conversation-created",pluginHandle,PURPLE_CALLBACK(conversationCreatedCb),NULL);
	purple_signal_connect(purple_conversations_get_handle(),"deleting-conversation",pluginHandle,PURPLE_CALLBACK(conversationDeletedCb),NULL);

	/** Connecting callback functions for adding/removing a friend to/from the group when he joins/leaves*/
	purple_signal_connect(purple_conversations_get_handle(),"chat-buddy-joined",pluginHandle,PURPLE_CALLBACK(buddyJoinedCb),NULL);
	purple_signal_connect(purple_conversations_get_handle(),"chat-buddy-left",pluginHandle,PURPLE_CALLBACK(buddyLeftCb),NULL);

	/** Eventloop for evaluating messages*/
	purple_timeout_add_seconds(1,(GSourceFunc)evaluateMessagesCb,NULL);

	return TRUE;
}

/** For specific notes on the meanings of each of these members, consult the C Plugin Howto
*  on the website.
*/
static PurplePluginInfo info = {
	PURPLE_PLUGIN_MAGIC,
	PURPLE_MAJOR_VERSION,
	PURPLE_MINOR_VERSION,
	PURPLE_PLUGIN_STANDARD,
	NULL,
	0,
	NULL,
	PURPLE_PRIORITY_DEFAULT,

	PLUGIN_ID,
	PLUGIN_NAME,
	PLUGIN_VERSION,
	"Pidgin plugin for F2F Network",
	"Pidgin plugin for F2F Network",
	PLUGIN_AUTHOR,
	"http://ulno.net",

	pluginLoad,
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
	pluginActions,		/* this tells libpurple the address of the function to call
				   to get the list of plugin actions.*/
	NULL,
	NULL,
	NULL,
	NULL
};

/** Called when plugin is inited*/
static void
initPlugin (PurplePlugin* plugin)
{
	Py_Initialize();
	//TODO: Something better that just "name"
	f2fPeer = f2fInit("name","");
	messages = g_async_queue_new();

	// add me
	// temporary for testing
	Friend* me = malloc(sizeof(Friend));
	me->name = malloc(strlen(f2fPeer->identifier)+1);
	strcpy(me->name,f2fPeer->identifier);
	me->protocol = "local";
	me->me = malloc(strlen(f2fPeer->identifier)+1);
	strcpy(me->me,f2fPeer->identifier);
	int myLocalId = getOrAddFriend(me);	//this should be 0
	//

}

PURPLE_INIT_PLUGIN (f2fplugin, initPlugin, info)
