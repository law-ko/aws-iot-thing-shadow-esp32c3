/*
 * ESP32-C3 Featured FreeRTOS IoT Integration V202204.00
 * Copyright (C) 2022 Amazon.com, Inc. or its affiliates.  All Rights Reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy of
 * this software and associated documentation files (the "Software"), to deal in
 * the Software without restriction, including without limitation the rights to
 * use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of
 * the Software, and to permit persons to whom the Software is furnished to do so,
 * subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS
 * FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
 * COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
 * IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 * https://www.FreeRTOS.org
 * https://github.com/FreeRTOS
 *
 */

/*
 * This file demonstrates a task which use the coreMQTT-agent API
 * to send and receive MQTT payloads and showcases how these can be used with
 * hardware.
 *
 * The created task is an instance of the task implemented by
 * prvTempSubPubAndLEDControlTask(). prvTempSubPubAndLEDControlTask()
 * subscribes to a topic then periodically reads the temperature sensor of the
 * ESP32-C3 and publishes a JSON payload with the temperature data to the same
 * topic to which it has subscribed. The user can also publish a JSON payload to
 * this same topic to turn off and on the LED on the ESP32-C3.
 * The command context sent to MQTTAgent_Publish() contains a unique number that
 * is sent back to the task as a task notification from the callback function
 * that executes when the PUBLISH operation is acknowledged (or just sent
 * in the case of QoS 0). The task checks the number it receives from the
 * callback equals the number it previously set in the command context before
 * printing out either a success or failure message.
 */

/* Includes *******************************************************************/

/* Standard includes. */
#include <string.h>
#include <stdio.h>
#include <assert.h>

/* FreeRTOS includes. */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/event_groups.h"

/* ESP-IDF includes. */
#include "esp_log.h"
#include "esp_event.h"
#include "sdkconfig.h"

/* coreMQTT library include. */
#include "core_mqtt.h"

/* coreMQTT-Agent include. */
#include "core_mqtt_agent.h"

/* coreMQTT-Agent network manager include. */
#include "core_mqtt_agent_manager.h"
#include "core_mqtt_agent_manager_events.h"

/* coreJSON include. */
#include "core_json.h"

/* Subscription manager include. */
#include "subscription_manager.h"

/* Hardware drivers include. */
#include "app_driver.h"

/* Public functions include. */
#include "temp_sub_pub_and_led_control_demo.h"

/* Demo task configurations include. */
#include "temp_sub_pub_and_led_control_demo_config.h"

/* SHADOW API header. */
#include "shadow.h"

/* Preprocessor definitions ***************************************************/

/* coreMQTT-Agent event group bit definitions */
#define CORE_MQTT_AGENT_CONNECTED_BIT              ( 1 << 0 )
#define CORE_MQTT_AGENT_OTA_NOT_IN_PROGRESS_BIT    ( 1 << 1 )

/* The maximum queue size for publish tasks. */
#define MAX_QUEUE_SIZE 5

/* Struct definitions *********************************************************/

/**
 * @brief Defines the structure to use as the command callback context in this
 * demo.
 */
struct MQTTAgentCommandContext
{
    MQTTStatus_t xReturnStatus;
    TaskHandle_t xTaskToNotify;
    uint32_t ulNotificationValue;
    void * pArgs;
};

typedef enum
{
    TEMPERATURE = 0,
	LED = 1
} update_t;

/* Global variables ***********************************************************/

/**
 * @brief Logging tag for ESP-IDF logging functions.
 */
const static char * TAG = "temp_sub_pub_and_led_control_demo";

/**
 * @brief The MQTT agent manages the MQTT contexts.  This set the handle to the
 * context used by this demo.
 */
extern MQTTAgentContext_t xGlobalMqttAgentContext;

/**
 * @brief The buffer to hold the publish topic filter. 
 * The topic is generated at runtime by adding the thing shadow names.
 *
 * @note The topic strings must persist until unsubscribed.
 */
static char pubTopicBuf[ temppubsubandledcontrolconfigSTRING_BUFFER_LENGTH ];

/**
 * @brief The buffer to hold the topic filter. The topic is generated at runtime
 * by adding the task names.
 *
 * @note The topic strings must persist until unsubscribed.
 */
static char subTopicBuf[ temppubsubandledcontrolconfigSTRING_BUFFER_LENGTH ];

/**
 * @brief The event group used to manage coreMQTT-Agent events.
 */
static EventGroupHandle_t xNetworkEventGroup;

/**
 * @brief The semaphore used to lock access to ulMessageID to eliminate a race
 * condition in which multiple tasks try to increment/get ulMessageID.
 */
static SemaphoreHandle_t xMessageIdSemaphore;

/**
 * @brief The message ID for the next message sent by this demo.
 */
static uint32_t ulMessageId = 0;

/**
 * @brief The buffer to hold the publish payload for temperature data.
 */
float temperatureValue;

/**
 * @brief The buffer to hold the state of the LED control data.
 */
uint32_t state = 0;

/**
 * @brief The queue to notify the publish message type and publish is needed to be sent.
 */
QueueHandle_t xQueue;

/* Static function declarations ***********************************************/

/**
 * @brief ESP Event Loop library handler for coreMQTT-Agent events.
 *
 * This handles events defined in core_mqtt_agent_events.h.
 */
static void prvCoreMqttAgentEventHandler( void * pvHandlerArg,
                                          esp_event_base_t xEventBase,
                                          int32_t lEventId,
                                          void * pvEventData );

/**
 * @brief Passed into MQTTAgent_Subscribe() as the callback to execute when the
 * broker ACKs the SUBSCRIBE message.  Its implementation sends a notification
 * to the task that called MQTTAgent_Subscribe() to let the task know the
 * SUBSCRIBE operation completed.  It also sets the xReturnStatus of the
 * structure passed in as the command's context to the value of the
 * xReturnStatus parameter - which enables the task to check the status of the
 * operation.
 *
 * See https://freertos.org/mqtt/mqtt-agent-demo.html#example_mqtt_api_call
 *
 * @param[in] pxCommandContext Context of the initial command.
 * @param[in].xReturnStatus The result of the command.
 */
static void prvSubscribeCommandCallback( MQTTAgentCommandContext_t * pxCommandContext,
                                         MQTTAgentReturnInfo_t * pxReturnInfo );

/**
 * @brief Passed into MQTTAgent_Publish() as the callback to execute when the
 * broker ACKs the PUBLISH message.  Its implementation sends a notification
 * to the task that called MQTTAgent_Publish() to let the task know the
 * PUBLISH operation completed.  It also sets the xReturnStatus of the
 * structure passed in as the command's context to the value of the
 * xReturnStatus parameter - which enables the task to check the status of the
 * operation.
 *
 * See https://freertos.org/mqtt/mqtt-agent-demo.html#example_mqtt_api_call
 *
 * @param[in] pxCommandContext Context of the initial command.
 * @param[in].xReturnStatus The result of the command.
 */
static void prvPublishCommandCallback( MQTTAgentCommandContext_t * pxCommandContext,
                                       MQTTAgentReturnInfo_t * pxReturnInfo );

/**
 * @brief Called by the task to wait for a notification from a callback function
 * after the task first executes either MQTTAgent_Publish()* or
 * MQTTAgent_Subscribe().
 *
 * See https://freertos.org/mqtt/mqtt-agent-demo.html#example_mqtt_api_call
 *
 * @param[in] pxCommandContext Context of the initial command.
 * @param[out] pulNotifiedValue The task's notification value after it receives
 * a notification from the callback.
 *
 * @return pdTRUE if the task received a notification, otherwise pdFALSE.
 */
static BaseType_t prvWaitForCommandAcknowledgment( uint32_t * pulNotifiedValue );

/**
 * @brief Passed into MQTTAgent_Subscribe() as the callback to execute when
 * there is an incoming publish on the topic being subscribed to.  Its
 * implementation just logs information about the incoming publish including
 * the publish messages source topic and payload.
 *
 * See https://freertos.org/mqtt/mqtt-agent-demo.html#example_mqtt_api_call
 *
 * @param[in] pvIncomingPublishCallbackContext Context of the initial command.
 * @param[in] pxPublishInfo Deserialized publish.
 */
static void prvIncomingPublishCallback( void * pvIncomingPublishCallbackContext,
                                        MQTTPublishInfo_t * pxPublishInfo );

/**
 * @brief Publish to the topic the demo task will also publish to - that
 * results in all outgoing publishes being published back to the task
 * (effectively echoed back).
 *
 * @param[in] xQoS The quality of service (QoS) to use.  Can be zero or one
 * for all MQTT brokers.  Can also be QoS2 if supported by the broker.  AWS IoT
 * does not support QoS2.
 */
static void prvPublishToTopic( MQTTQoS_t xQoS,
                               char * pcTopicName,
                               char * pcPayload );

/**
 * @brief Subscribe to the topic the demo task will also publish to - that
 * results in all outgoing publishes being published back to the task
 * (effectively echoed back).
 *
 * @param[in] xQoS The quality of service (QoS) to use.  Can be zero or one
 * for all MQTT brokers.  Can also be QoS2 if supported by the broker.  AWS IoT
 * does not support QoS2.
 */
static bool prvSubscribeToTopic( MQTTQoS_t xQoS,
                                 char * pcTopicFilter );

/**
 * @brief The function that implements the task demonstrated by this file.
 */
static void prvTempSubPubAndLEDControlTask( void * pvParameters );

/* Static function definitions ************************************************/

static void prvPublishCommandCallback( MQTTAgentCommandContext_t * pxCommandContext,
                                       MQTTAgentReturnInfo_t * pxReturnInfo )
{
    /* Store the result in the application defined context so the task that
     * initiated the publish can check the operation's status. */
    pxCommandContext->xReturnStatus = pxReturnInfo->returnCode;

    if( pxCommandContext->xTaskToNotify != NULL )
    {
        /* Send the context's ulNotificationValue as the notification value so
         * the receiving task can check the value it set in the context matches
         * the value it receives in the notification. */
        xTaskNotify( pxCommandContext->xTaskToNotify,
                     pxCommandContext->ulNotificationValue,
                     eSetValueWithOverwrite );
    }
}

static void prvSubscribeCommandCallback( MQTTAgentCommandContext_t * pxCommandContext,
                                         MQTTAgentReturnInfo_t * pxReturnInfo )
{
    bool xSubscriptionAdded = false;
    MQTTAgentSubscribeArgs_t * pxSubscribeArgs = ( MQTTAgentSubscribeArgs_t * ) pxCommandContext->pArgs;

    /* Store the result in the application defined context so the task that
     * initiated the subscribe can check the operation's status.  Also send the
     * status as the notification value.  These things are just done for
     * demonstration purposes. */
    pxCommandContext->xReturnStatus = pxReturnInfo->returnCode;

    /* Check if the subscribe operation is a success. Only one topic is
     * subscribed by this demo. */
    if( pxReturnInfo->returnCode == MQTTSuccess )
    {
        /* Add subscription so that incoming publishes are routed to the application
         * callback. */
        xSubscriptionAdded = addSubscription( ( SubscriptionElement_t * ) xGlobalMqttAgentContext.pIncomingCallbackContext,
                                              pxSubscribeArgs->pSubscribeInfo->pTopicFilter,
                                              pxSubscribeArgs->pSubscribeInfo->topicFilterLength,
                                              prvIncomingPublishCallback,
                                              NULL );

        if( xSubscriptionAdded == false )
        {
            ESP_LOGE( TAG,
                      "Failed to register an incoming publish callback for topic %.*s.",
                      pxSubscribeArgs->pSubscribeInfo->topicFilterLength,
                      pxSubscribeArgs->pSubscribeInfo->pTopicFilter );
        }
    }

    xTaskNotify( pxCommandContext->xTaskToNotify,
                 ( uint32_t ) ( pxReturnInfo->returnCode ),
                 eSetValueWithOverwrite );
}

static BaseType_t prvWaitForCommandAcknowledgment( uint32_t * pulNotifiedValue )
{
    BaseType_t xReturn;

    /* Wait for this task to get notified, passing out the value it gets
     * notified with. */
    xReturn = xTaskNotifyWait( 0,
                               0,
                               pulNotifiedValue,
                               portMAX_DELAY );
    return xReturn;
}
static void prvParseIncomingPublish( MQTTPublishInfo_t * pMqttPublishInfo )
{
    char * outValue = NULL;
    uint32_t outValueLength = 0U;
    JSONStatus_t result = JSONSuccess;
	ShadowStatus_t shadowStatus = SHADOW_FAIL;
	ShadowMessageType_t messageType = ShadowMessageTypeMaxNum;
	bool stateChanged = false;
	
	shadowStatus = Shadow_MatchTopicString(	pMqttPublishInfo->pTopicName,
											pMqttPublishInfo->topicNameLength,
											&messageType,
											NULL,
											NULL,
											NULL,
											NULL );

	if( shadowStatus == SHADOW_SUCCESS )
	{
		if( messageType == ShadowMessageTypeUpdateDelta )
		{
			ESP_LOGI( TAG, "Received a delta message: \"%.*s\"", pMqttPublishInfo->payloadLength, pMqttPublishInfo->pPayload );

			/* Check if JSON is valid. */
			result = JSON_Validate( ( const char * ) pMqttPublishInfo->pPayload,
									pMqttPublishInfo->payloadLength );

			/* Update device LED based on delta request. */
			if( result == JSONSuccess )
			{
				result = JSON_Search( ( char * ) pMqttPublishInfo->pPayload,
									  pMqttPublishInfo->payloadLength,
									  "state.led.power",
									  sizeof( "state.led.power" ) - 1,
									  &outValue,
									  ( size_t * ) &outValueLength );
			}
			else
			{
				ESP_LOGE( TAG, "Invalid JSON." );
			}

			if ( result == JSONSuccess )
			{
				/* Convert the extracted value to an unsigned integer value. */
				state = ( uint32_t ) strtoul( outValue, NULL, 10 );

				if( state == 1 )
				{
					ESP_LOGI( TAG, "Turning on LED." );
					ws2812_led_set_rgb( 0, 25, 0 );
					stateChanged = true;
				}
				else if( state == 0 )
				{
					ESP_LOGI( TAG, "Turning off LED." );
					ws2812_led_clear();
					stateChanged = true;
				}
			}
			else
			{
				/* JSON is valid, but the publish is not related to LED. */
				ESP_LOGE( TAG, "No power key in JSON object." );
			}

			/* Update the device shadow if the state changed. */
			if( stateChanged == true )
			{
				ESP_LOGI( TAG, "Sending update to device shadow." );

				update_t updateType = LED;
				xQueueSend( xQueue, &updateType, 0 );
			}
			
		}
		else if( messageType == ShadowMessageTypeUpdateAccepted )
		{
			ESP_LOGI( TAG, "Received an update accepted message." );
		}
		else if( messageType == ShadowMessageTypeUpdateDocuments )
		{
			ESP_LOGI( TAG, "Received an update documents message." );
		}
		else if( messageType == ShadowMessageTypeUpdateRejected )
		{
			ESP_LOGI( TAG, "Received an update rejected message." );
		}
		else
		{
			ESP_LOGE( TAG, "Received an unknown message." );
		}
	}
}

static void prvIncomingPublishCallback( void * pvIncomingPublishCallbackContext,
                                        MQTTPublishInfo_t * pxPublishInfo )
{
    static char cTerminatedString[ temppubsubandledcontrolconfigSTRING_BUFFER_LENGTH ];

    ( void ) pvIncomingPublishCallbackContext;

    /* Create a message that contains the incoming MQTT payload to the logger,
     * terminating the string first. */
    if( pxPublishInfo->payloadLength < temppubsubandledcontrolconfigSTRING_BUFFER_LENGTH )
    {
        memcpy( ( void * ) cTerminatedString, pxPublishInfo->pPayload, pxPublishInfo->payloadLength );
        cTerminatedString[ pxPublishInfo->payloadLength ] = 0x00;
    }
    else
    {
        memcpy( ( void * ) cTerminatedString, pxPublishInfo->pPayload, temppubsubandledcontrolconfigSTRING_BUFFER_LENGTH );
        cTerminatedString[ temppubsubandledcontrolconfigSTRING_BUFFER_LENGTH - 1 ] = 0x00;
    }

    ESP_LOGI( TAG,
              "Received incoming publish message %s",
              cTerminatedString );

    // prvParseIncomingPublish( ( char * ) pxPublishInfo->pPayload, pxPublishInfo->payloadLength );
	prvParseIncomingPublish( pxPublishInfo );
}

static void prvPublishToTopic( MQTTQoS_t xQoS,
                               char * pcTopicName,
                               char * pcPayload )
{
	uint32_t ulPublishMessageId = 0;

    MQTTStatus_t xCommandAdded;
    BaseType_t xCommandAcknowledged = pdFALSE;

    MQTTPublishInfo_t xPublishInfo = { 0 };

    MQTTAgentCommandContext_t xCommandContext = { 0 };
    MQTTAgentCommandInfo_t xCommandParams = { 0 };

	uint32_t ulNotification = 0U, ulValueToNotify = 0UL;

	/* Create a unique number of the publish that is about to be sent.  The number
     * is used as the command context and is sent back to this task as a notification
     * in the callback that executed upon receipt of the subscription acknowledgment.
     * That way this task can match an acknowledgment to a subscription. */
    xTaskNotifyStateClear( NULL );

	/* Create a unique number for the publish that is about to be sent.
     * This number is used in the command context and is sent back to this task
     * as a notification in the callback that's executed upon receipt of the
     * publish from coreMQTT-Agent.
     * That way this task can match an acknowledgment to the message being sent.
     */
    xSemaphoreTake( xMessageIdSemaphore, portMAX_DELAY );
    {
        ++ulMessageId;
        ulPublishMessageId = ulMessageId;
    }
    xSemaphoreGive( xMessageIdSemaphore );

    /* Configure the publish operation. The topic name string must persist for
     * duration of publish! */
    xPublishInfo.qos = xQoS;
    xPublishInfo.pTopicName = pcTopicName;
    xPublishInfo.topicNameLength = ( uint16_t ) strlen( pcTopicName );
    xPublishInfo.pPayload = pcPayload;
    xPublishInfo.payloadLength = ( uint16_t ) strlen( pcPayload );

    /* Complete an application defined context associated with this publish
     * message.
     * This gets updated in the callback function so the variable must persist
     * until the callback executes. */
    xCommandContext.ulNotificationValue = ulPublishMessageId;
    xCommandContext.xTaskToNotify = xTaskGetCurrentTaskHandle();

    xCommandParams.blockTimeMs = temppubsubandledcontrolconfigMAX_COMMAND_SEND_BLOCK_TIME_MS;
    xCommandParams.cmdCompleteCallback = prvPublishCommandCallback;
    xCommandParams.pCmdCompleteCallbackContext = &xCommandContext;

    do
    {
        /* Wait for coreMQTT-Agent task to have working network connection and
         * not be performing an OTA update. */
        xEventGroupWaitBits( xNetworkEventGroup,
                             CORE_MQTT_AGENT_CONNECTED_BIT | CORE_MQTT_AGENT_OTA_NOT_IN_PROGRESS_BIT,
                             pdFALSE,
                             pdTRUE,
                             portMAX_DELAY );

        ESP_LOGI( TAG,
                  "Task \"%s\" sending publish request to coreMQTT-Agent with message \"%s\" on topic \"%s\".",
                  pcTaskGetName( xCommandContext.xTaskToNotify ),
                  pcPayload,
                  pcTopicName );

		/* To ensure ulNotification doesn't accidentally hold the expected value
         * as it is to be checked against the value sent from the callback.. */
        ulNotification = ~ulValueToNotify;

        xCommandAcknowledged = pdFALSE;

        xCommandAdded = MQTTAgent_Publish( &xGlobalMqttAgentContext,
                                           &xPublishInfo,
                                           &xCommandParams );

        if( xCommandAdded == MQTTSuccess )
        {
            /* For QoS 1 and 2, wait for the publish acknowledgment.  For QoS0,
             * wait for the publish to be sent. */
            ESP_LOGI( TAG,
                      "Task \"%s\" waiting for publish %d to complete.",
                      pcTaskGetName( xCommandContext.xTaskToNotify ),
                      ulPublishMessageId );

            xCommandAcknowledged = prvWaitForCommandAcknowledgment( &ulNotification );
        }
        else
        {
            ESP_LOGE( TAG,
                      "Failed to enqueue publish command. Error code=%s",
                      MQTT_Status_strerror( xCommandAdded ) );
        }

        /* Check all ways the status was passed back just for demonstration
         * purposes. */
		// @warning ulNotifiedValue is not the same as ulPublishMessageId, bypassing it first
		ESP_LOGW( TAG, "xCommandAcknowledged=%s, xCommandContext.xReturnStatus=%s, ulNotifiedValue=%d, ulPublishMessageId=%d", (xCommandAcknowledged==pdTRUE) ? "pdTRUE" : "pdFALSE", (xCommandContext.xReturnStatus==MQTTSuccess) ? "MQTTSuccess" : "MQTTFail", ulNotification, ulValueToNotify);
        // if( ( xCommandAcknowledged != pdTRUE ) ||
        //     ( xCommandContext.xReturnStatus != MQTTSuccess ) ||
        //     ( ulNotification != ulValueToNotify ) )
		if( ( xCommandAcknowledged != pdTRUE ) ||
            ( xCommandContext.xReturnStatus != MQTTSuccess )  )
        {
            ESP_LOGW( TAG,
                      "Error or timed out waiting for ack for publish message %ld. Re-attempting publish.",
                      ulValueToNotify );
        }
        else
        {
            ESP_LOGI( TAG,
                      "Publish %ld succeeded for task \"%s\".",
                      ulValueToNotify,
                      pcTaskGetName( xCommandContext.xTaskToNotify ) );
        }
    // } while( ( xCommandAcknowledged != pdTRUE ) ||
    //          ( xCommandContext.xReturnStatus != MQTTSuccess ) ||
    //          ( ulNotification != ulValueToNotify ) );
	} while( ( xCommandAcknowledged != pdTRUE ) ||
             ( xCommandContext.xReturnStatus != MQTTSuccess ) );
}

static bool prvSubscribeToTopic( MQTTQoS_t xQoS,
                                 char * pcTopicFilter )
{
    MQTTStatus_t xCommandAdded;
    BaseType_t xCommandAcknowledged = pdFALSE;
    MQTTAgentSubscribeArgs_t xSubscribeArgs;
    MQTTSubscribeInfo_t xSubscribeInfo;
    static int32_t ulNextSubscribeMessageID = 0;
    MQTTAgentCommandContext_t xApplicationDefinedContext = { 0UL };
    MQTTAgentCommandInfo_t xCommandParams = { 0UL };

    /* Create a unique number of the subscribe that is about to be sent.  The number
     * is used as the command context and is sent back to this task as a notification
     * in the callback that executed upon receipt of the subscription acknowledgment.
     * That way this task can match an acknowledgment to a subscription. */
    xTaskNotifyStateClear( NULL );

    ulNextSubscribeMessageID++;

    /* Complete the subscribe information.  The topic string must persist for
     * duration of subscription! */
    xSubscribeInfo.pTopicFilter = pcTopicFilter;
    xSubscribeInfo.topicFilterLength = ( uint16_t ) strlen( pcTopicFilter );
    xSubscribeInfo.qos = xQoS;
    xSubscribeArgs.pSubscribeInfo = &xSubscribeInfo;
    xSubscribeArgs.numSubscriptions = 1;

    /* Complete an application defined context associated with this subscribe message.
     * This gets updated in the callback function so the variable must persist until
     * the callback executes. */
    xApplicationDefinedContext.ulNotificationValue = ulNextSubscribeMessageID;
    xApplicationDefinedContext.xTaskToNotify = xTaskGetCurrentTaskHandle();
    xApplicationDefinedContext.pArgs = ( void * ) &xSubscribeArgs;

    xCommandParams.blockTimeMs = temppubsubandledcontrolconfigMAX_COMMAND_SEND_BLOCK_TIME_MS;
    xCommandParams.cmdCompleteCallback = prvSubscribeCommandCallback;
    xCommandParams.pCmdCompleteCallbackContext = ( void * ) &xApplicationDefinedContext;

    /* Loop in case the queue used to communicate with the MQTT agent is full and
     * attempts to post to it time out.  The queue will not become full if the
     * priority of the MQTT agent task is higher than the priority of the task
     * calling this function. */
    ESP_LOGI( TAG,
              "Sending subscribe request to agent for topic filter: %s with id %d",
              pcTopicFilter,
              ( int ) ulNextSubscribeMessageID );

    do
    {
        xCommandAdded = MQTTAgent_Subscribe( &xGlobalMqttAgentContext,
                                             &xSubscribeArgs,
                                             &xCommandParams );
    } while( xCommandAdded != MQTTSuccess );

    /* Wait for acks to the subscribe message - this is optional but done here
     * so the code below can check the notification sent by the callback matches
     * the ulNextSubscribeMessageID value set in the context above. */
    xCommandAcknowledged = prvWaitForCommandAcknowledgment( NULL );

    /* Check both ways the status was passed back just for demonstration
     * purposes. */
    if( ( xCommandAcknowledged != pdTRUE ) ||
        ( xApplicationDefinedContext.xReturnStatus != MQTTSuccess ) )
    {
        ESP_LOGE( TAG,
                  "Error or timed out waiting for ack to subscribe message topic %s",
                  pcTopicFilter );
    }
    else
    {
        ESP_LOGI( TAG,
                  "Received subscribe ack for topic %s containing ID %d",
                  pcTopicFilter,
                  ( int ) xApplicationDefinedContext.ulNotificationValue );
    }

    return xCommandAcknowledged;
}

static void prvTempSubPubAndLEDControlTask( void * pvParameters )
{
    MQTTPublishInfo_t xPublishInfo = { 0UL };
    char payloadBuf[ temppubsubandledcontrolconfigSTRING_BUFFER_LENGTH ];
    MQTTAgentCommandContext_t xCommandContext;
    uint32_t ulNotification = 0U, ulValueToNotify = 0UL;
    MQTTStatus_t xCommandAdded;
    MQTTQoS_t xQoS;
    MQTTAgentCommandInfo_t xCommandParams = { 0UL };
    char * pcSubTopicBuffer = subTopicBuf;
	char * pcPubTopicBuffer = pubTopicBuf;
    const char * pcTaskName;
    uint32_t ulPublishPassCounts = 0;
    uint32_t ulPublishFailCounts = 0;

    pcTaskName = pcTaskGetName( xTaskGetCurrentTaskHandle() );

    /* Hardware initialisation */
    app_driver_init();

	xMessageIdSemaphore = xSemaphoreCreateMutex();

    /* Initialize the coreMQTT-Agent event group. */
    xNetworkEventGroup = xEventGroupCreate();
    xEventGroupSetBits( xNetworkEventGroup,
                        CORE_MQTT_AGENT_OTA_NOT_IN_PROGRESS_BIT );

    /* Register coreMQTT-Agent event handler. */
    xCoreMqttAgentManagerRegisterHandler( prvCoreMqttAgentEventHandler );

    xQoS = ( MQTTQoS_t ) temppubsubandledcontrolconfigQOS_LEVEL;

    /* Create a topic name for this task to subscribe to. */
	uint16_t outLength = 0;
	Shadow_AssembleTopicString(	ShadowTopicStringTypeUpdateDelta,
								CONFIG_GRI_THING_NAME,
								sizeof( CONFIG_GRI_THING_NAME ) - 1,
								"",	// for classic shadow
								0,  // for classic shadow
								pcSubTopicBuffer,
								temppubsubandledcontrolconfigSTRING_BUFFER_LENGTH,
								&outLength );
	pcSubTopicBuffer[outLength] = '\0'; // null terminate the string for correct strlen

    /* Subscribe to the same topic to which this task will publish.  That will
     * result in each published message being published from the server back to
     * the target. */
    prvSubscribeToTopic( xQoS, pcSubTopicBuffer );

    /* Configure the publish operation. */
    memset( ( void * ) &xPublishInfo, 0x00, sizeof( xPublishInfo ) );
    xPublishInfo.qos = xQoS;
    xPublishInfo.pTopicName = pcPubTopicBuffer;
    xPublishInfo.pPayload = payloadBuf;

	/* Create a topic name for this task to publish to. */
	Shadow_AssembleTopicString(	ShadowTopicStringTypeUpdate,
							CONFIG_GRI_THING_NAME,
							sizeof( CONFIG_GRI_THING_NAME ) - 1,
							"",	// for classic shadow
							0,  // for classic shadow
							pcPubTopicBuffer,
							temppubsubandledcontrolconfigSTRING_BUFFER_LENGTH,
							&outLength );
	pcPubTopicBuffer[outLength] = '\0'; // null terminate the string for correct strlen
	xPublishInfo.topicNameLength = ( uint16_t ) outLength;

    /* Store the handler to this task in the command context so the callback
     * that executes when the command is acknowledged can send a notification
     * back to this task. */
    memset( ( void * ) &xCommandContext, 0x00, sizeof( xCommandContext ) );
    xCommandContext.xTaskToNotify = xTaskGetCurrentTaskHandle();

    xCommandParams.blockTimeMs = temppubsubandledcontrolconfigMAX_COMMAND_SEND_BLOCK_TIME_MS;
    xCommandParams.cmdCompleteCallback = prvPublishCommandCallback;
    xCommandParams.pCmdCompleteCallbackContext = &xCommandContext;

    ulValueToNotify = 0UL;

	/* Update initial report status. */
	state = 1;
	update_t updateType = LED;
	xQueueSend( xQueue, &updateType, 0 );

    /* For an infinite number of publishes */
    while( 1 )
    {

		/* Wait for incoming publish request. */
		if ( xQueueReceive( xQueue, &updateType, portMAX_DELAY ) == pdPASS )
		{
			/* Put LED as first priority in if/else statement to minimize INSTANT SYNC response time. */
			if ( updateType == LED )
			{
				/* Create a payload for INSTANT SYNC response. */
				snprintf( 	payloadBuf,
							temppubsubandledcontrolconfigSTRING_BUFFER_LENGTH,
							"{"                          \
							"\"state\":{"                \
							"\"reported\":{"             \
							"\"led\":"                   \
							"{"                          \
							" \"power\": %d"             \
							"}"                          \
							"}"                          \
							"}"                          \
							"}"                          \
							,
							state ); 
			}
			else if ( updateType == TEMPERATURE )
			{
				/* Create a payload to send with the publish message.  This contains
				* the task name, temperature and the iteration number. */
				snprintf( payloadBuf,
						temppubsubandledcontrolconfigSTRING_BUFFER_LENGTH,
						"{"                          \
						"\"state\":{"                \
						"\"reported\":{"             \
						"\"temperatureSensor\":"     \
						"{"                          \
						" \"taskName\": \"%s\","     \
						" \"temperatureValue\": %f," \
						" \"iteration\": %d"         \
						"}"                          \
						"}"                          \
						"}"                          \
						"}"                          \
						,
						pcTaskName,
						temperatureValue,
						( int ) ulValueToNotify );
			}
		}

		

		// prvPublishToTopic( xQoS, pcPubTopicBuffer, payloadBuf );
        xPublishInfo.payloadLength = ( uint16_t ) strlen( payloadBuf );


        /* Also store the incrementing number in the command context so it can
         * be accessed by the callback that executes when the publish operation
         * is acknowledged. */
        xCommandContext.ulNotificationValue = ulValueToNotify;

        /* Wait for coreMQTT-Agent task to have working network connection and
         * not be performing an OTA update. */
        xEventGroupWaitBits( xNetworkEventGroup,
                             CORE_MQTT_AGENT_CONNECTED_BIT | CORE_MQTT_AGENT_OTA_NOT_IN_PROGRESS_BIT,
                             pdFALSE,
                             pdTRUE,
                             portMAX_DELAY );

        ESP_LOGI( TAG,
                  "Sending publish request to agent with message \"%s\" on topic \"%s\"",
                  payloadBuf,
                  pcPubTopicBuffer );

        /* To ensure ulNotification doesn't accidentally hold the expected value
         * as it is to be checked against the value sent from the callback.. */
        ulNotification = ~ulValueToNotify;

        xCommandAdded = MQTTAgent_Publish( &xGlobalMqttAgentContext,
                                           &xPublishInfo,
                                           &xCommandParams );
        configASSERT( xCommandAdded == MQTTSuccess );

        /* For QoS 1 and 2, wait for the publish acknowledgment.  For QoS0,
         * wait for the publish to be sent. */
        ESP_LOGI( TAG,
                  "Task %s waiting for publish %d to complete.",
                  pcTaskName,
                  ulValueToNotify );

        prvWaitForCommandAcknowledgment( &ulNotification );

        /* The value received by the callback that executed when the publish was
         * acked came from the context passed into MQTTAgent_Publish() above, so
         * should match the value set in the context above. */
        if( ulNotification == ulValueToNotify )
        {
            ulPublishPassCounts++;
            ESP_LOGI( TAG,
                      "Rx'ed %s from Tx to %s (P%d:F%d).",
                      ( xQoS == 0 ) ? "completion notification for QoS0 publish" : "ack for QoS1 publish",
                      pcPubTopicBuffer,
                      ulPublishPassCounts,
                      ulPublishFailCounts );
        }
        else
        {
            ulPublishFailCounts++;
            ESP_LOGE( TAG,
                      "Timed out Rx'ing %s from Tx to %s (P%d:F%d)",
                      ( xQoS == 0 ) ? "completion notification for QoS0 publish" : "ack for QoS1 publish",
                      pcPubTopicBuffer,
                      ulPublishPassCounts,
                      ulPublishFailCounts );
        }

        ulValueToNotify++;
    }

    vTaskDelete( NULL );
}

static void prvTempUpdateTask( void * pvParameters )
{
	while (1)
	{
		temperatureValue = app_driver_temp_sensor_read_celsius();

		update_t updateType = TEMPERATURE;
		xQueueSend( xQueue, &updateType, 0 );

		/* Add a little randomness into the delay so the tasks don't remain
         * in lockstep. */
        TickType_t xTicksToDelay = 	pdMS_TO_TICKS( temppubsubandledcontrolconfigDELAY_BETWEEN_PUBLISH_OPERATIONS_MS ) +
                        			( rand() % 0xff );

        vTaskDelay( xTicksToDelay );
	}
	

}

static void prvCoreMqttAgentEventHandler( void * pvHandlerArg,
                                          esp_event_base_t xEventBase,
                                          int32_t lEventId,
                                          void * pvEventData )
{
    ( void ) pvHandlerArg;
    ( void ) xEventBase;
    ( void ) pvEventData;

    switch( lEventId )
    {
        case CORE_MQTT_AGENT_CONNECTED_EVENT:
            ESP_LOGI( TAG,
                      "coreMQTT-Agent connected." );
            xEventGroupSetBits( xNetworkEventGroup,
                                CORE_MQTT_AGENT_CONNECTED_BIT );
            break;

        case CORE_MQTT_AGENT_DISCONNECTED_EVENT:
            ESP_LOGI( TAG,
                      "coreMQTT-Agent disconnected. Preventing coreMQTT-Agent "
                      "commands from being enqueued." );
            xEventGroupClearBits( xNetworkEventGroup,
                                  CORE_MQTT_AGENT_CONNECTED_BIT );
            break;

        case CORE_MQTT_AGENT_OTA_STARTED_EVENT:
            ESP_LOGI( TAG,
                      "OTA started. Preventing coreMQTT-Agent commands from "
                      "being enqueued." );
            xEventGroupClearBits( xNetworkEventGroup,
                                  CORE_MQTT_AGENT_OTA_NOT_IN_PROGRESS_BIT );
            break;

        case CORE_MQTT_AGENT_OTA_STOPPED_EVENT:
            ESP_LOGI( TAG,
                      "OTA stopped. No longer preventing coreMQTT-Agent "
                      "commands from being enqueued." );
            xEventGroupSetBits( xNetworkEventGroup,
                                CORE_MQTT_AGENT_OTA_NOT_IN_PROGRESS_BIT );
            break;

        default:
            ESP_LOGE( TAG,
                      "coreMQTT-Agent event handler received unexpected event: %d",
                      lEventId );
            break;
    }
}

/* Public function definitions ************************************************/

void vStartTempSubPubAndLEDControlDemo( void )
{
	xQueue = xQueueCreate( MAX_QUEUE_SIZE, sizeof( update_t ) );

    xTaskCreate( prvTempSubPubAndLEDControlTask,
                 "TempSubPubLED",
                 temppubsubandledcontrolconfigTASK_STACK_SIZE,
                 NULL,
                 temppubsubandledcontrolconfigTASK_PRIORITY,
                 NULL );

	xTaskCreate( prvTempUpdateTask,
				 "TempUpdate",
				 3072,
				 NULL,
				 5,
				 NULL );
}
