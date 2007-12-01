/*
 * psserverchar.cpp
 *
 * Copyright (C) 2002 Atomic Blue (info@planeshift.it, http://www.atomicblue.org)
 *
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation (version 2 of the License)
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.
 *
 * Communicates with the client side version of charmanager.
 */
#include <psconfig.h>

#include <iutil/databuff.h>
#include <iutil/object.h>
#include <csutil/csstring.h>

#include "util/serverconsole.h"
#include "util/psxmlparser.h"
#include "util/log.h"
#include "util/mathscript.h"
#include "util/psconst.h"
#include "util/eventmanager.h"

#include "rpgrules/factions.h"

#include "net/message.h"
#include "net/messages.h"
#include "net/msghandler.h"
#include "net/charmessages.h"

#include "engine/celbase.h"

#include "bulkobjects/pscharacter.h"
#include "bulkobjects/pscharacterloader.h"
#include "bulkobjects/psitem.h"
#include "bulkobjects/pstrade.h"
#include "bulkobjects/psraceinfo.h"
#include "bulkobjects/pssectorinfo.h"
#include "bulkobjects/psmerchantinfo.h"
#include "bulkobjects/psactionlocationinfo.h"

#include "globals.h"

#include "psserverchar.h"
#include "client.h"
#include "playergroup.h"
#include "clients.h"
#include "util/psdatabase.h"
#include "psserver.h"
#include "chatmanager.h"
#include "groupmanager.h"
#include "spellmanager.h"
#include "slotmanager.h"
#include "workmanager.h"
#include "netmanager.h"
#include "cachemanager.h"
#include "progressionmanager.h"
#include "creationmanager.h"
#include "exchangemanager.h"
#include "actionmanager.h"
#include "serverstatus.h"

///This expresses in seconds how many days the char hasn't logon. 60 days, at the moment.
#define MAX_DAYS_NO_LOGON 5184000 

/// The number of characters per email account
#define CHARACTERS_ALLOWED 4       

psServerCharManager::psServerCharManager()
{
    slotManager = NULL;

    calc_item_merchant_price_buy = psserver->GetMathScriptEngine()->FindScript("Calc Item Merchant Price Buy");
    if (calc_item_merchant_price_buy)
    {
        calc_item_merchant_price_item_price_buy = calc_item_merchant_price_buy->GetOrCreateVar("ItemPrice");
        calc_item_merchant_price_char_data_buy = calc_item_merchant_price_buy->GetOrCreateVar("CharData");
        calc_item_merchant_price_char_result_buy = calc_item_merchant_price_buy->GetOrCreateVar("Result");
    }

    calc_item_merchant_price_sell = psserver->GetMathScriptEngine()->FindScript("Calc Item Merchant Price Sell");
    if (calc_item_merchant_price_sell)
    {
        calc_item_merchant_price_item_price_sell = calc_item_merchant_price_sell->GetOrCreateVar("ItemPrice");
        calc_item_merchant_price_char_data_sell = calc_item_merchant_price_sell->GetOrCreateVar("CharData");
        calc_item_merchant_price_char_result_sell = calc_item_merchant_price_sell->GetOrCreateVar("Result");
    }
}

psServerCharManager::~psServerCharManager()
{
    if (psserver->GetEventManager())
    {
        psserver->GetEventManager()->Unsubscribe(this, MSGTYPE_CHAR_INFO);
        psserver->GetEventManager()->Unsubscribe(this, MSGTYPE_GUIINVENTORY);
        psserver->GetEventManager()->Unsubscribe(this, MSGTYPE_GUIMERCHANT);
        psserver->GetEventManager()->Unsubscribe(this, MSGTYPE_VIEW_ITEM);
        psserver->GetEventManager()->Unsubscribe(this, MSGTYPE_VIEW_SKETCH);
        psserver->GetEventManager()->Unsubscribe(this, MSGTYPE_WRITE_BOOK);
        psserver->GetEventManager()->Unsubscribe(this, MSGTYPE_FACTION_INFO);
    }

    delete slotManager;
    slotManager = NULL;
}

bool psServerCharManager::Initialize( ClientConnectionSet* ccs)
{
    clients = ccs;

    psserver->GetEventManager()->Subscribe(this, MSGTYPE_CHAR_INFO,REQUIRE_READY_CLIENT);
    psserver->GetEventManager()->Subscribe(this, MSGTYPE_GUIINVENTORY,NO_VALIDATION);
    psserver->GetEventManager()->Subscribe(this, MSGTYPE_GUIMERCHANT,REQUIRE_READY_CLIENT|REQUIRE_ALIVE);
    psserver->GetEventManager()->Subscribe(this, MSGTYPE_VIEW_ITEM,REQUIRE_READY_CLIENT);
    psserver->GetEventManager()->Subscribe(this, MSGTYPE_VIEW_SKETCH,REQUIRE_READY_CLIENT);
    psserver->GetEventManager()->Subscribe(this, MSGTYPE_WRITE_BOOK, REQUIRE_READY_CLIENT);
    psserver->GetEventManager()->Subscribe(this, MSGTYPE_FACTION_INFO);

    slotManager = new SlotManager;
    if ( !(slotManager && slotManager->Initialize()) )
        return false;

    return true;
}

void psServerCharManager::ViewItem( MsgEntry* me )
{
    psViewItemDescription mesg(me);
    Client* client = clients->Find(me->clientnum);
    ViewItem(client, mesg.containerID, (INVENTORY_SLOT_NUMBER) mesg.slotID);
}

void psServerCharManager::UpdateSketch( MsgEntry* me )
{
    psSketchMessage sketchMsg(me);

    if (sketchMsg.valid)
    {
        Client *client = clients->Find(me->clientnum);
        psItem *item   = client->GetCharacterData()->Inventory().FindItemID(sketchMsg.ItemID);
        if (item)
        {
            // TODO: Probably need to validate the xml here somehow
            item->GetBaseStats()->SetSketch(csString(sketchMsg.Sketch));
            printf("Updated sketch for item %u to: %s\n", sketchMsg.ItemID, sketchMsg.Sketch.GetDataSafe());
            psserver->SendSystemInfo(me->clientnum, "Your drawing has been updated.");
            item->GetBaseStats()->Save();
        }
        else
        {
            Error3("Item %u not found in sketch definition message from client %s.",sketchMsg.ItemID, client->GetName() );
        }
    }
}

void psServerCharManager::ViewItem(Client* client, int containerID, INVENTORY_SLOT_NUMBER slotID)
{
    // printf("Viewing item in Container %d, slot %d.\n", containerID, slotID);

    psItem* item = slotManager->FindItem(client, containerID, slotID);
        
    if ( !item )
    {
        psActionLocation *action = psserver->GetActionManager()->FindAction( containerID );
        if ( !action )
        {
            Error3( "No item/action : %d, %d", containerID, slotID );
            return;
        }
        else
        {
            // Check for container
            if ( action->IsContainer() )
            {
                // Get container instance
                uint32 instance_id = action->GetInstanceID();
                gemItem* realItem = GEMSupervisor::GetSingleton().FindItemEntity( instance_id );
                if (!realItem)
                {
                    Error3("Invalid instance ID %u in action location %s", instance_id, action->name.GetDataSafe());
                    return;
                }

                // Get item pointer
                item = realItem->GetItem();
                if ( !item )
                {
                    CPrintf (CON_ERROR, "Invalid ItemID in Action Location Response.\n");
                    return;
                }

                // Check range ignoring Y co-ordinate
                csWeakRef<gemObject> gem = client->GetActor();
                if (gem.IsValid() && gem->RangeTo(realItem, true) > RANGE_TO_SELECT)
                {
                    psserver->SendSystemError(client->GetClientNum(),
                        "You are not in range to see %s.",item->GetName());
                    return;
                }
                SendActionContents( client, action, item );
            }
       
            // Check for minigames or entrances
            else if (action->IsGameBoard() || action->IsEntrance())
            {
                csString description = action->GetDescription();
                if (!description.IsEmpty())
                {
                    psViewActionLocationMessage mesg(client->GetClientNum(), action->name, description.GetData());
                    mesg.SendMessage();
                }
                else
                {
                    Error2("Action location %s XML response does not have a valid description", action->name.GetData());
                    return;
                }
            }
            else
            {
                psViewActionLocationMessage mesg(client->GetClientNum(), action->name, action->response);
                mesg.SendMessage();
            }
        }
    }
    else // we're inspecting an item here
    {
        if ( item->GetIsContainer() )
        {
            SendContainerContents(client, item, slotID);
        }

        // for now, just examining mind item gives craft information rather then its normal description
        //  eventually /study will give same results after some delay
        // check if we are examining an item that is meant for mind slot
        else if( item->FitsInSlots(PSITEMSTATS_SLOT_MIND) ) 
        {
            //We pass through container, slot & parent IDs so that we can pass them back and forth when writing books.
            SendCraftTransInfo(client, item);
        }

        //for now, we pretend that /examine reads.  When we implement /read, this will change to only send the description of the book
        //e.g. "This book is bound in a leathery Pterosaur hide and branded with the insignia of the Sunshine Squadron"
        //NOTE that this logic is sensitive to ordering; Books currently have a nonzero "sketch" length since that's where they 
        //store their content.         
        else if( item->GetBaseStats()->GetIsReadable() ) 
        {
            //We pass through container, slot & parent IDs so that we can pass them back and forth when writing books.
            SendBookText(client, item, containerID, slotID);
        }
        else if (item->GetBaseStats()->GetCreative() == PSITEMSTATS_CREATIVETYPE_SKETCH &&
                 item->GetBaseStats()->GetSketch().Length() > 0)   // Sketch
        {
            SendSketchDefinition(item,client);
        }
        else // Not a container, show description
        {
            SendItemDescription( client, item );
        }
    }
}

void psServerCharManager::SendSketchDefinition(psItem *item, Client *client)
{
    // Get character capabilities
    static MathScript *script;
    if (!script)
        script = psserver->GetMathScriptEngine()->FindScript("Calc Player Sketch Limits");
    if (!script)
    {
        Error1("Cannot find mathscript: Calc Player Sketch Limits");
        return;
    }

    static MathScriptVar *var;
    static MathScriptVar *score;
    static MathScriptVar *count;

    if (!var)
    {
        var = script->GetOrCreateVar("Actor");
        score = script->GetOrCreateVar("IconScore");
        count = script->GetOrCreateVar("PrimCount");
    }

    var->SetObject(client->GetActor()->GetCharacterData() );  // Now the mathscript can access all info about player
    script->Execute();
    int playerScore = (int)score->GetValue();
    int primCount   = (int)count->GetValue();

    // Build the limit string for the client
    csString xml("<limits>");
    xml.AppendFmt("<count>%d</count>",primCount);  // This limits how many things you can add on the client.

    // If the player is not the crafter, and a crafter is specified, then the player cannot edit the item
    if (item->GetCrafterID() && item->GetCrafterID() != (uint)client->GetActor()->GetPlayerID())
        xml.Append("<rdonly/>");

    size_t i=0;
    while (CacheManager::GetSingleton().GetLimitation(i))
    {
        const psCharacterLimitation *charlimit = CacheManager::GetSingleton().GetLimitation(i);

        // This limits which icons each player can use to only the ones below his level.
        if (playerScore > charlimit->min_score)
            xml.AppendFmt("<ic>%s</ic>",charlimit->value.GetDataSafe() );
        i++;
    }
    xml += "</limits>";

    // Now send all this
    psSketchMessage msg( client->GetClientNum(), item->GetUID(), 0, xml, item->GetBaseStats()->GetSketch(), item->GetBaseStats()->IsThisTheCreator(client->GetCharacterData()->GetCharacterID()) );
    msg.SendMessage();
}

void psServerCharManager::HandleBookWrite(MsgEntry* me, Client* client)
{
    psWriteBookMessage mesg(me);

    //if we're getting this, it's gotta be a request or a save
    if(mesg.messagetype == mesg.REQUEST)
    {
         psItem* item = slotManager->FindItem(client, mesg.containerID, (INVENTORY_SLOT_NUMBER) mesg.slotID);

         //is it a writable book?  In our inventory? Are we the author?
         if(item && item->GetBaseStats()->GetIsWriteable() && 
            item->GetOwningCharacter() == client->GetCharacterData() &&
            item->GetBaseStats()->IsThisTheCreator(client->GetCharacterData()->GetCharacterID()))
         {
              //We could maybe let the work manager know that we're busy writing something
              //or track that this is the book we're working on, and only allow saves to a 
              //book that was opened for writing.  This would be a good thing.
              //Also check for other writing in progress
              csString theText(item->GetBookText());
              csString theTitle(item->GetName());
              psWriteBookMessage resp(client->GetClientNum(), theTitle, theText, true,  (INVENTORY_SLOT_NUMBER)mesg.slotID, mesg.containerID);
              resp.SendMessage();
            //  CPrintf(CON_DEBUG, "Sent: %s\n",resp.ToString(NULL).GetDataSafe());
         } 
         else 
         {
            //construct error message indicating that the item is not editable
            psserver->SendSystemError(client->GetClientNum(), "You cannot write on this item.");
         }
    } 
    else if (mesg.messagetype == mesg.SAVE)
    {
       // CPrintf(CON_DEBUG, "Attempt to save book in slot id %d\n",mesg.slotID);
        //something like:
        psItem* item = slotManager->FindItem(client, mesg.containerID, (INVENTORY_SLOT_NUMBER) mesg.slotID);
        if(item && item->GetBaseStats()->GetIsWriteable())
        {
            // check title is still unique
            csString currentTitle = item->GetName();
            if (mesg.title.Length() > 0)
            {
                uint32 existingItemID = CacheManager::GetSingleton().BasicItemStatsByNameExist(mesg.title);
                if (existingItemID != 0 && existingItemID != item->GetBaseStats()->GetUID())
                {
                    psserver->SendSystemError(me->clientnum, "The title is not unique");
                }
                else if (mesg.title != currentTitle)
                {
                    currentTitle = mesg.title;
                    item->GetBaseStats()->SetName(mesg.title);
                    item->GetBaseStats()->SaveName();
                }
            }

            // or psItem* item = (find the player)->GetCurrentWritingItem();
            item->SetBookText(mesg.content);
            psserver->SendSystemInfo(me->clientnum, "You have written in \'%s\'.", currentTitle.GetDataSafe());
        }
        // clear current writing item
        
    }
}

void psServerCharManager::HandleMessage( MsgEntry* me, Client *client )
{
    client = clients->FindAny(me->clientnum);

    if (!client)
    {
        CPrintf (CON_ERROR, "***Couldn't find clientnum in serverchar HandleMessage\n");
        return;
    }

    switch ( me->GetType() )
    {
		// Case to handle incoming faction requests from a client. 
		// These should normally only be a request for a full 
		// list
		case MSGTYPE_FACTION_INFO:
		{
			HandleFaction(me);
			return;
		}

        case MSGTYPE_GUIINVENTORY:
        {
            HandleInventoryMessage(me);
            return;
        }

        case MSGTYPE_VIEW_ITEM:
        {
            // printf("Got msgtype view item\n");
            ViewItem(me);
            break;
        }

        case MSGTYPE_VIEW_SKETCH:
        {
            UpdateSketch(me);
            break;
        }
        //Not yet implemented, using /examine for now 
        case MSGTYPE_READ_BOOK:
        {
            ViewItem( me );
            break;
        }
        
        case MSGTYPE_WRITE_BOOK:
        {
           HandleBookWrite( me, client );
           break;
        }

        case MSGTYPE_CHAR_INFO:
        {
            HandleCharInfo( me, client );
            return;
        }
        case MSGTYPE_GUIMERCHANT:
        {
            HandleMerchantMessage(me, client);
            break;
        }
    }
}


void psServerCharManager::HandleFaction(MsgEntry* me)
{
    Client* client = clients->Find(me->clientnum);
    if (client==NULL)
        return;

    psCharacter *chardata=client->GetCharacterData();
    if (chardata==NULL)	
		return;

	
	psFactionMessage outMsg(me->clientnum, psFactionMessage::MSG_FULL_LIST);

	csHash<FactionStanding*, int>::GlobalIterator iter(chardata->GetActor()->GetFactions()->GetStandings().GetIterator());
    while(iter.HasNext())
    {
        FactionStanding* standing = iter.Next();
		outMsg.AddFaction(standing->faction->name, standing->score);
    }

	outMsg.BuildMsg();
	outMsg.SendMessage();
}


//---------------------------------------------------------------------------
/**
 * This handles all formats of inventory message.
 */
//---------------------------------------------------------------------------
bool psServerCharManager::HandleInventoryMessage( MsgEntry* me )
{
    if ( !me )
        return false;

    psGUIInventoryMessage incoming(me);
    int     fromClientNumber    = me->clientnum;

    switch ( incoming.command )
    {
        case psGUIInventoryMessage::REQUEST:
        case psGUIInventoryMessage::UPDATE_REQUEST:
        {
            SendInventory(fromClientNumber, 
                  (static_cast<psGUIInventoryMessage::commands>(incoming.command)==psGUIInventoryMessage::UPDATE_REQUEST));
            break;
        }
    }
    return true;
}

bool psServerCharManager::SendInventory( int clientNum, bool sendUpdatesOnly)
{
    psGUIInventoryMessage* outgoing;
    
    Client* client = clients->Find(clientNum);
    if (client==NULL)
        return false;

    bool exchanging = (client->GetExchangeID() != 0); // When exchanging, only send partial inv of what is not offered
 
    int toClientNumber = clientNum;
//    int itemCount, itemRemovedCount;
//    unsigned int z;

    psCharacter *chardata=client->GetCharacterData();
    if (chardata==NULL)
        return false;

    outgoing = new psGUIInventoryMessage(toClientNumber,
                                         psGUIInventoryMessage::LIST,
                                         (uint32_t)chardata->Inventory().GetInventoryIndexCount()-1,  // skip item 0
                                         (uint32_t)0, chardata->Inventory().MaxWeight() );

    Notify2(LOG_EXCHANGES,"Sending %zu items...\n", chardata->Inventory().GetInventoryIndexCount()-1);
    for (size_t i=1; i < chardata->Inventory().GetInventoryIndexCount(); i++)
    {
        psCharacterInventory::psCharacterInventoryItem *invitem = chardata->Inventory().GetIndexCharInventoryItem(i);
        psItem *item  = invitem->GetItem();

        int slot  = item->GetLocInParent(true);

        int invType = CONTAINER_INVENTORY_BULK;

        if (slot < PSCHARACTER_SLOT_BULK1)  // equipped if < than first bulk
            invType = CONTAINER_INVENTORY_EQUIPMENT;

        Notify5(LOG_EXCHANGES, "  Inv item %s, slot %d, weight %1.1f, stack count %u\n",item->GetName(), slot, item->GetWeight(), item->GetStackCount() );
        outgoing->AddItem(item->GetName(),
                          invType,
                          slot,
                          (exchanging && invitem->exchangeOfferSlot != PSCHARACTER_SLOT_NONE) ? item->GetStackCount() - invitem->exchangeStackCount : item->GetStackCount(),
                          item->GetWeight(),
                          item->GetTotalStackSize(),
                          item->GetImageName(),
                          item->GetPurifyStatus());
    }

    psMoney m = chardata->Money();
    Exchange *exchange = exchanging ? psserver->exchangemanager->GetExchange(client->GetExchangeID()) : NULL;
    if (exchange)
        m += -exchange->GetOfferedMoney(client);
    outgoing->AddMoney(m);

    if (outgoing->valid)
    {
        outgoing->msg->ClipToCurrentSize();
        psserver->GetEventManager()->SendMessage(outgoing->msg);

        // server now can believe the clients inventory cache is upto date
        //        inventoryCache->SetCacheStatus(psCache::VALID);
    }
    else
    {
        Bug2("Could not create valid psGUIInventoryMessage for client %u.\n",toClientNumber);
    }

    delete outgoing;

    return true;
}

bool psServerCharManager::UpdateItemViews( int clientNum )
{
    Client* client = clients->Find(clientNum);

    // If inventory window is up, update it
    SendInventory( clientNum );

    // If glyph window is up, update it
    psserver->GetSpellManager()->SendGlyphs(client);
    
    if ( slotManager->worldContainer )
    {
        SendContainerContents(client, slotManager->worldContainer->GetItem(), slotManager->containerEntityID);
        slotManager->worldContainer = NULL;
        slotManager->containerEntityID = 0;
    } 
    return true;
}

bool psServerCharManager::IsBanned(const char* name)
{
    // Check if the name is banned
    csString nName = NormalizeCharacterName(name);
    for(int i = 0;i < (int)CacheManager::GetSingleton().GetBadNamesCount(); i++)
    {
        csString name = CacheManager::GetSingleton().GetBadName(i); // Name already normalized
        if(name == nName)
            return true;
    }
    return false;
}


bool psServerCharManager::HasConnected( csString name )
{
    int secondsLastLogin;
    secondsLastLogin = 0;

    //Query to the db that calculates already the amount of seconds since the last login.
    Result result(db->Select("SELECT last_login, UNIX_TIMESTAMP() - UNIX_TIMESTAMP(last_login) as seconds_since_last_login FROM characters WHERE name = '%s'", name.GetData() ));
    //There is no character with such a name. 
    if (!result.IsValid() || result.Count() == 0)
    {
        return false;
    }
    //We check when the char was last online.    
    secondsLastLogin = result[0].GetInt(1);

    if ( secondsLastLogin > MAX_DAYS_NO_LOGON ) // More than 2 months since last login.
    { 
        return false;
    }
    
    //The char has connected recently.
    return true;
}


void psServerCharManager::SendCraftTransInfo(Client *client, psItem *item)
{
    csString mess;
    psItemStats* mindStats = item->GetCurrentStats();
    if ( !mindStats )
    {
        Error1("No stats for mind item.");
        return;
    }

    // Get character pointer
    if ( client == NULL )
    {
        Error1("Bad client pointer.");
        return;
    }

    psCharacter* character = client->GetCharacterData();
    if ( character == NULL )
    {
        Error1("Bad client psCharacter pointer.");
        return;
    }

    // Get craft combo info string based on skill levels
    csString* combString = GetComboInfoString(character,mindStats->GetUID());
    mess.Append(combString->GetData());
    mess.Append("\n\n");

    // Get transformation info string based on skill levels
    csString* transString = GetTransInfoString(character,mindStats->GetUID());
    mess.Append(transString->GetData());
    mess.Append(".\n");

    // Send info to cleint
    psMsgCraftingInfo msg(client->GetClientNum(),mess);
    if (msg.valid)
        psserver->GetEventManager()->SendMessage(msg.msg);
    else
        Bug2("Could not create valid psMsgCraftingInfo for client %u.\n",client->GetClientNum());
}

csString* psServerCharManager::GetComboInfoString(psCharacter* character, uint32 designID)
{
    csString* combString = new csString("");
    CraftComboInfo* combInfo = CacheManager::GetSingleton().GetTradeComboInfoByItemID(designID);
    if ( combInfo == NULL )
    {
        return combString;
    }

    // If any skill check fails then do not display this combinations string
    csArray<CraftSkills*>* skillArray = combInfo->skillArray;
    for ( int count = 0; count<(int)skillArray->GetSize(); count++ )
    {
        // Check if craft step minimum primary skill level is meet by client
        int priSkill = skillArray->Get(count)->priSkillId;
        if(priSkill != 0)
        {
            if(skillArray->Get(count)->minPriSkill >= (int)character->GetSkills()->GetSkillRank((PSSKILL)priSkill))
            {
                return combString;
            }
        }

        // Check if craft step minimum secondary skill level is meet by client
        int secSkill = skillArray->Get(count)->secSkillId;
        if(secSkill != 0)
        {
            if(skillArray->Get(count)->minSecSkill >= (int)character->GetSkills()->GetSkillRank((PSSKILL)priSkill))
            {
                return combString;
            }
        }
    }

    // Otherwise send combination string
    combString->Append(combInfo->craftCombDescription.GetData());
    return combString;
}

csString* psServerCharManager::GetTransInfoString(psCharacter* character, uint32 designID)
{
    csString* transString = new csString("");
    csArray<CraftTransInfo*>* craftArray = CacheManager::GetSingleton().GetTradeTransInfoByItemID(designID);
    if ( craftArray == NULL )
    {
        return transString;
    }

    for ( int count = 0; count<(int)craftArray->GetSize(); count++ )
    {
        // Check if craft step minimum primary skill level is meet by client
        int priSkill = craftArray->Get(count)->priSkillId;
        if(priSkill != 0)
        {
            if(craftArray->Get(count)->minPriSkill >= (int)character->GetSkills()->GetSkillRank((PSSKILL)priSkill))
            {
                continue;
            }
        }

        // Check if craft step minimum seconday skill level is meet by client
        int secSkill = craftArray->Get(count)->secSkillId;
        if(secSkill != 0)
        {
            if(craftArray->Get(count)->minSecSkill >= (int)character->GetSkills()->GetSkillRank((PSSKILL)secSkill))
            {
                continue;
            }
        }

        // Otherwise tack on trasnformation step description to message
        transString->Append(craftArray->Get(count)->craftStepDescription.GetData());
        transString->Append("\n");
    }
    return transString;
}

void psServerCharManager::HandleCharInfo( MsgEntry* me, Client *client )
{
    psCharInfoRequestMessage irmsg(me);

    psCharInfoRequestMessage::type
    command = psCharInfoRequestMessage::SKILL_REQUEST_ALL;

    if ( irmsg.command == command )
    {
        psCharacter *chardata=client->GetCharacterData();
        if (chardata==NULL)
            return;
        int i;
        csString outData("<SL>");
        csString tempBuffer;

        for (i=0;i<PSSKILL_COUNT;i++)
        {
            psSkillInfo *skillinfo=CacheManager::GetSingleton().GetSkillByID((PSSKILL)i);
            if (skillinfo!=NULL)
            {
        csString escpxml = EscpXML(skillinfo->name);
                tempBuffer.Format( "<SKILL NAME=\"%s\" VALUE=\"%iu\" />",
                                   escpxml.GetData(),
                                   chardata->GetSkills()->GetSkillRank((PSSKILL)i));
                outData.Append(tempBuffer);
            }
        }
        outData.Append("</SL>");

         psCharInfoRequestMessage msg(me->clientnum, command, outData);
         if (msg.valid)
            psserver->GetEventManager()->SendMessage(msg.msg);
        else
        {
            Bug2("Could not create valid psCharInfoRequestMessage for client %u.\n",me->clientnum);
        }

    }
}


void psServerCharManager::BeginTrading(Client * client, gemObject * target, const csString & type)
{
    psCharacter * merchant = NULL;
    int clientnum = client->GetClientNum();
    psCharacter* character = client->GetCharacterData();

    // Make sure that we are not busy with something else
    if (client->GetActor()->GetMode() != PSCHARACTER_MODE_PEACE)
    {
        psserver->SendSystemError(client->GetClientNum(), "You cannot trade because you are already busy.");
        return;
    }

    merchant = target->GetCharacterData();
    if(!merchant)
    {
        psserver->SendSystemInfo(client->GetClientNum(), "Merchant not found.");
        return;
    }

    if (client->GetActor()->RangeTo(target) > RANGE_TO_SELECT)
    {
        psserver->SendSystemInfo(client->GetClientNum(), "You are not in range to trade with %s.",merchant->GetCharName());
        return;
    }

    if (!target->IsAlive())
    {
        psserver->SendSystemInfo(client->GetClientNum(), "Can't trade with a dead merchant.");
        return;
    }

    if (!merchant->IsMerchant())
    {
        psserver->SendSystemInfo(client->GetClientNum(), "%s isn't a merchant.",merchant->GetCharName());
        return;
    }

    psserver->SendSystemInfo(client->GetClientNum(), "You started trading with %s.",merchant->GetCharName());

    if (type == "SELL")
    {
        csString commandData;
        commandData.Format("<MERCHANT ID=\"%d\" TRADE_CMD=\"%d\" />",
                merchant->GetCharacterID(),psGUIMerchantMessage::SELL);

        psGUIMerchantMessage msg1(clientnum,psGUIMerchantMessage::MERCHANT,commandData);
        msg1.SendMessage();
        character->SetTradingStatus(psCharacter::SELLING,merchant);
    }
    else
    {
        csString commandData;
        commandData.Format("<MERCHANT ID=\"%d\" TRADE_CMD=\"%d\" />",
                merchant->GetCharacterID(),psGUIMerchantMessage::BUY);
        psGUIMerchantMessage msg1(clientnum,psGUIMerchantMessage::MERCHANT,commandData);
        psserver->GetEventManager()->SendMessage(msg1.msg);
        character->SetTradingStatus(psCharacter::BUYING,merchant);
    }

    // Build category list
    csString categoryList("<L>");
    csString buff;

    psMerchantInfo * merchantInfo = merchant->GetMerchantInfo();

    for ( size_t z = 0; z < merchantInfo->categories.GetSize(); z++ )
    {
        psItemCategory * category = merchantInfo->categories[z];
    csString escpxml = EscpXML(category->name);
        buff.Format("<CATEGORY ID=\"%d\" "
                    "NAME=\"%s\" />",category->id,
                    escpxml.GetData());
        categoryList.Append(buff);
    }
    categoryList.Append("</L>");

    psGUIMerchantMessage msg2(clientnum,psGUIMerchantMessage::CATEGORIES,categoryList.GetData());
    if (msg2.valid)
        psserver->GetEventManager()->SendMessage(msg2.msg);
    else
    {
        Bug2("Could not create valid psGUIMerchantMessage for client %u.\n",clientnum);
    }


    SendPlayerMoney(client);
}

void psServerCharManager::HandleMerchantRequest(psGUIMerchantMessage& msg, Client *client)
{
    csRef<iDocumentNode> exchangeNode = ParseString(msg.commandData, "R");
    if (!exchangeNode)
        return;

    csRef<iDocumentAttribute> attr = exchangeNode->GetAttribute("TARGET");
    csString type = exchangeNode->GetAttributeValue("TYPE");

    gemObject * target = NULL;
    if (attr)
    {
        csString targetName = attr->GetValue();
        target = GEMSupervisor::GetSingleton().FindObject(targetName);
        if (!target)
        {
            psserver->SendSystemInfo(client->GetClientNum(), "Merchant '%s' not found.", targetName.GetData());
            return;
        }
    }
    else
    {
        target = client->GetTargetObject();
        if (!target)
        {
            psserver->SendSystemInfo(client->GetClientNum(), "You have no target selected.");
            return;
        }
    }

    BeginTrading(client, target, type);
}

void psServerCharManager::HandleMerchantCategory(psGUIMerchantMessage& msg, Client *client)
{
    psCharacter* character = client->GetCharacterData();

    csRef<iDocumentNode> merchantNode = ParseString (msg.commandData, "C");
    if (!merchantNode)
        return;

    psCharacter * merchant;
    psMerchantInfo * merchantInfo;

    if (VerifyTrade(client, character,&merchant,&merchantInfo,
        "category","",merchantNode->GetAttributeValueAsInt("ID")))
    {
        csString category = merchantNode->GetAttributeValue("CATEGORY");
        psItemCategory * itemCategory = merchantInfo->FindCategory(category);
        if (!itemCategory)
        {
            CPrintf(CON_DEBUG, "Player %s fails to get items in category %s. Unkown category!\n",
                character->GetCharName(), (const char*)category);
            return;
        }
        if (!merchant->GetActor()->IsAlive())
        {
            psserver->SendSystemInfo(client->GetClientNum(), "You can't trade with a dead merchant.");
            return;
        }

        // Send item list for given category
        if (character->GetTradingStatus() == psCharacter::BUYING)
        {
            SendMerchantItems( client, merchant, itemCategory );
        }
        else
        {
            SendPlayerItems( client, itemCategory );
        }
    }
}

void psServerCharManager::HandleMerchantBuy(psGUIMerchantMessage& msg, Client *client)
{
    psCharacter* character = client->GetCharacterData();
    csRef <iDocumentNode> merchantNode = ParseString(msg.commandData, "T");
    if (!merchantNode)
        return;

    csString itemName = merchantNode->GetAttributeValue("ITEM");
    int count = merchantNode->GetAttributeValueAsInt("COUNT");
    int merchantID = merchantNode->GetAttributeValueAsInt("ID");
    uint32 itemID = (uint32)merchantNode->GetAttributeValueAsInt("ITEM_ID");

    // check negative item counts to avoid integer overflow
    if (count <= 0)
    {
        psserver->SendSystemError(client->GetClientNum(), "You can't trade this amount of items.");
        return;
    }

    psCharacter * merchant;
    psMerchantInfo * merchantInfo;

    if (VerifyTrade(client, character,&merchant,&merchantInfo,
        "buy",itemName,merchantID))
    {
        psItem * item = merchant->Inventory().FindItemID(itemID);
        if (!item || count > item->GetStackCount())
        {
            psserver->SendSystemError(client->GetClientNum(), "Merchant does not have %i %s.", count, itemName.GetData());
            return;
        }
        if (!merchant->GetActor()->IsAlive())
        {
            psserver->SendSystemError(client->GetClientNum(),"That merchant is dead");
            return;
        }
        psMoney price = CalculateMerchantPrice(item, client, false);
        psMoney money = character->Money();

        if (price*count > money)
        {
            psserver->SendSystemError(client->GetClientNum(),"You need more money");
            return;
        }

        int canFit = (int)character->Inventory().HowManyCanFit(item); // count that actually fit into buyer's inventory

        if (count > canFit)
        {
            count = canFit;
            // Notify the buyer that their inventory is full.  (will purchase what fits)
            psserver->SendSystemError(client->GetClientNum(),"Your inventory is full");
            if (count <= 0) return;
        }

        // if item is to be personalised, then duplicate the item_stats and personalise
        // by given it a unique name for the purchaser.
        psItem* newitem;
        if (item->GetBuyPersonalise())
        {
            // only 1 personalised thing can be bought at a time
            if (count != 1)
            {
                psserver->SendSystemError(client->GetClientNum(),"You can only purchase 1 Personalised thing at a time!");
                return;
            }

            // copy 'item' item_stats to create unique item...
            // personalised name is "<item> of <purchaser>"
            // If "<item> of <purchaser>" already exists, add " (<number>)"
            // (being the row count+1 of item_stats)
            // to the name to ensure uniqueness.
            csString personalisedName(item->GetName());
            personalisedName.AppendFmt(" of %s", client->GetName());
            if (CacheManager::GetSingleton().BasicItemStatsByNameExist(personalisedName) > 0)
                personalisedName.AppendFmt(" (%zu)", CacheManager::GetSingleton().ItemStatsSize()+1);

            psItemStats* newCreation = CacheManager::GetSingleton().CopyItemStats(item->GetBaseStats()->GetUID(),
                                                                                  personalisedName);

            if (!newCreation)
               return;

            newCreation->SetCreator(character->GetCharacterID(), PSITEMSTATS_CREATOR_VALID);
            newCreation->Save();

            // instantiate it
            newitem = newCreation->InstantiateBasicItem();
            if (newitem)
               newitem->SetLoaded();
        }
        else // normal purchase
        {
            newitem = item->Copy(count);
        }

        if (newitem == NULL)
        {
            Error2("Error: failed to create item %s.", itemName.GetData());
            psserver->SendSystemError(client->GetClientNum(), "Error: failed to create item %s.", itemName.GetData());
            return;
        }
        // If we managed to buy some items, we pay some money

        bool stackable = newitem->GetBaseStats()->GetIsStackable();

        if (character->Inventory().Add(newitem, false, stackable))
        {
            psMoney cost;
            cost = (price * count).Normalized();

            character->SetMoney(money - cost);

            psserver->SendSystemOK( client->GetClientNum(), "You bought %d %s for %s a total of %d Trias.",
                count, itemName.GetData(), cost.ToUserString().GetDataSafe(),cost.GetTotal());

            psBuyEvent evt(
                character->GetCharacterID(),
                merchant->GetCharacterID(),
                item->GetName(),
                count,
                (int)item->GetCurrentStats()->GetQuality(),
                cost.GetTotal()
                );
            evt.FireEvent();
        }
        else
        {
            // No empty or stackable slot in bulk or any container
            psserver->SendSystemError(client->GetClientNum(),"You're carrying too many items");
            CacheManager::GetSingleton().RemoveInstance(newitem);
            return;
        }

        csString buf;
        buf.Format("%s, %s, %s, \"%s\", %d, %d", client->GetName(), merchant->name.GetData(), "Buy", 
        itemName.GetData(), count, (price * count).GetTotal());
        psserver->GetLogCSV()->Write(CSV_EXCHANGES, buf);

        // Update client views
        SendPlayerMoney( client );
        SendMerchantItems( client, merchant, item->GetCategory() );

        // Update all client views
        UpdateItemViews( client->GetClientNum() );
    }
}

void psServerCharManager::HandleMerchantSell(psGUIMerchantMessage& msg, Client *client)
{
    psCharacter* character = client->GetCharacterData();
    csRef <iDocumentNode> merchantNode = ParseString(msg.commandData, "T");
    if (!merchantNode)
        return;

    csString itemName = merchantNode->GetAttributeValue("ITEM");
    int count = merchantNode->GetAttributeValueAsInt("COUNT");
    int merchantID = merchantNode->GetAttributeValueAsInt("ID");

    // check negative item counts to avoid integer overflow
    if (count <= 0)
    {
        psserver->SendSystemError(client->GetClientNum(), "You can't trade this amount of items.");
        return;
    }

    psCharacter * merchant;
    psMerchantInfo * merchantInfo;

    if (VerifyTrade(client, character,&merchant,&merchantInfo,
        "sell",itemName,merchantID))
    {
        uint32 itemID =(uint32) merchantNode->GetAttributeValueAsInt("ITEM_ID");
        psItem * item = character->Inventory().FindItemID(itemID);
        if (!item)
            return;
        if (character->Inventory().GetContainedItemCount(item) > 0)
        {
            psserver->SendSystemError(client->GetClientNum(), "You must take your items out of the container first.");
            return;
        }
        if (!merchant->GetActor()->IsAlive())
        {
            psserver->SendSystemError(client->GetClientNum(), "You can't trade with a dead merchant.");
            return;
        }

        psMoney price = CalculateMerchantPrice(item, client, true);
        psMoney money = character->Money();

        count = MIN(count, item->GetStackCount());
        csString name(item->GetName());

        item = character->Inventory().RemoveItem(NULL,item->GetLocInParent(true), count);
        if (item == NULL)
        {
            Error3("RemoveItem failed while selling to merchant %s %i", name.GetDataSafe(), count);
            return;
        }

        psMoney cost;
        cost = (price * count).Normalized();

        character->SetMoney(money + cost);

        psserver->SendSystemOK(client->GetClientNum(), "You sold %d %s for %s a total of %d Trias.",
                               count, itemName.GetData(), cost.ToUserString().GetDataSafe(),cost.GetTotal());

        // Record
        psSellEvent evt(character->GetCharacterID(),
                        merchant->GetCharacterID(),
                        item->GetName(),
                        count,
                        (int)item->GetCurrentStats()->GetQuality(),
                        cost.GetTotal() );
        evt.FireEvent();

        ServerStatus::sold_items += count;
        ServerStatus::sold_value += (price * count).GetTotal();

        csString buf;
        buf.Format("%s, %s, %s, \"%s\", %d, %d", client->GetName(), merchant->name.GetData(), "Sell", 
            itemName.GetData(), count, (price * count).GetTotal());
        psserver->GetLogCSV()->Write(CSV_EXCHANGES, buf);

        // Update client views
        SendPlayerMoney( client );
        SendPlayerItems( client, item->GetCategory() );

        // items are not currently given to merchant, they are just destroyed
        CacheManager::GetSingleton().RemoveInstance(item);

        // Update all client views
        UpdateItemViews( client->GetClientNum() );
    }
}

void psServerCharManager::HandleMerchantView(psGUIMerchantMessage& msg, Client *client)
{
    psCharacter* character = client->GetCharacterData();
    csRef <iDocumentNode> merchantNode = ParseString(msg.commandData, "V");
    if (!merchantNode)
        return;

    csString itemName = merchantNode->GetAttributeValue("ITEM");
    int merchantID    = merchantNode->GetAttributeValueAsInt("ID");
    uint32 itemID     = (uint32)merchantNode->GetAttributeValueAsInt("ITEM_ID");
    int tradeCommand  = merchantNode->GetAttributeValueAsInt("TRADE_CMD");

    psCharacter * merchant;
    psMerchantInfo * merchantInfo;

    if (VerifyTrade(client, character,&merchant,&merchantInfo,
        "view",itemName,merchantID))
    {
        if (!merchant->GetActor()->IsAlive())
        {
            psserver->SendSystemInfo(client->GetClientNum(), "You can't trade with a dead merchant.");
            return;
        }
        psItem * item;
        if (tradeCommand == psGUIMerchantMessage::SELL)
            item = character->Inventory().FindItemID(itemID);
        else
            item = merchant->Inventory().FindItemID(itemID);

        if (!item)
        {
            CPrintf(CON_DEBUG, "Player %s failed to view item %s. No item!\n",
                client->GetName(), (const char*)itemName);
            return;
        }

        // check for container items
        if (item->GetIsContainer())
        {
            // FIXME: I don't think you want to open the container window
            //        when buying an empty container.  We also don't want
            //        to allow selling non-empty containers...they probaly
            //        shouldn't even appear in the list...
            SendContainerContents(client, item);
        }
        else
        {
            SendItemDescription(client, item);
        }
    }
}

void psServerCharManager::HandleMerchantMessage( MsgEntry* me, Client *client )
{
    psGUIMerchantMessage msg(me);
    if (!msg.valid)
    {
        Debug2(LOG_NET,me->clientnum,"Received unparsable psGUIMerchantMessage from client %u.\n",me->clientnum);
        return;
    }

    //    CPrintf(CON_DEBUG, "psServerCharManager::HandleMerchantMessage (%s, %d,%s)\n",
    //           (const char*)client->GetName(),msg.command, (const char*)msg.commandData);

    switch (msg.command)
    {
        // This handles the initial request to buy or sell from a merchant.
        // A list of categories that this merchant handles is sent
        case psGUIMerchantMessage::REQUEST:
        {
            HandleMerchantRequest(msg,client);
            break;
        }
        //  This handles
        case psGUIMerchantMessage::CATEGORY:
        {
            HandleMerchantCategory(msg,client);
            break;
        }
        case psGUIMerchantMessage::BUY:
        {
            HandleMerchantBuy(msg,client);
            break;
        }
        case psGUIMerchantMessage::SELL:
        {
            HandleMerchantSell(msg,client);
            break;
        }
        case psGUIMerchantMessage::VIEW:
        {
            HandleMerchantView(msg,client);
            break;
        }
        case psGUIMerchantMessage::CANCEL:
        {
            client->GetCharacterData()->SetTradingStatus(psCharacter::NOT_TRADING,0);
            break;
        }

    }
}

bool psServerCharManager::VerifyTrade( Client * client, psCharacter * character, psCharacter ** merchant, psMerchantInfo ** info,
                                       const char * trade,const char * itemName, unsigned int merchantID)
{
    *merchant = character->GetMerchant();
    if (!*merchant)
    {
        psserver->SendSystemInfo(client->GetClientNum(),"You can only buy from a merchant.");
        Debug4(LOG_CHARACTER,client->GetClientNum(),"Player %s failed to %s item %s. No merchant!\n",
                character->GetCharName(), trade, itemName);
        return false;
    }
    *info = (*merchant)->GetMerchantInfo();
    if (!*info)
    {
        CPrintf(CON_DEBUG, "Player %s failed to %s item %s. No merchant info!\n",
                character->GetCharName(), trade, itemName);
        return false;
    }
    // Check if player is trading with this merchant.
    if (character->GetTradingStatus() == psCharacter::NOT_TRADING)
    {
        CPrintf(CON_DEBUG, "Player %s failed to %s item %s. No trading status!\n",
                character->GetCharName(), trade, itemName);
        return false;
    }
    // Check if this is correct merchant
    if (merchantID != (*merchant)->GetCharacterID())
    {
        CPrintf(CON_DEBUG, "Player %s failed to %s item %s. Different merchant!\n",
                character->GetCharName(), trade, itemName);
        return false;
    }
    // Check range
    if (character->GetActor()->RangeTo((*merchant)->GetActor()) > RANGE_TO_SELECT)
    {
        psserver->SendSystemInfo(client->GetClientNum(),"Merchant is out of range.");
        CPrintf(CON_DEBUG, "Player %s failed to %s item %s. Out of range!\n",
                character->GetCharName(), trade, itemName);
        return false;
    }

    return true;
}


void psServerCharManager::SendOutPlaySoundMessage( int clientnum, const char* itemsound, const char* action )
{
    if (clientnum == 0 || itemsound == NULL || action == NULL)
        return;

    csString sound = itemsound;

    if (sound == "item.nosound")
        return;

    sound += ".";
    sound += action;
    
    Debug3(LOG_SOUND,clientnum,"Sending sound %s to client %d", sound.GetData(), clientnum);

    psPlaySoundMessage msg(clientnum, sound);
    psserver->GetEventManager()->SendMessage(msg.msg);
    
// TODO: Sounds should really be multicasted, so others can hear them
//    psserver->GetEventManager()->Multicast(msg, fromClient->GetActor()->GetMulticastClients(), 0, range );
}

void psServerCharManager::SendOutEquipmentMessages( gemActor* actor,
                                                    INVENTORY_SLOT_NUMBER slot,
                                                    psItem* item,
                                                    int equipped )
{
    PS_ID eid = actor->GetEntity()->GetID();

    csString mesh = item->GetMeshName();
    csString part = item->GetPartName();
    csString partMesh = item->GetPartMeshName();

    // If we're doing a 'deequip', there is no texture.
    csString texture;
    if(equipped != psEquipmentMessage::DEEQUIP)
        texture = item->GetTextureName();

    // printf("Sending Equipment Message for %s, slot %d, item %s.\n", actor->GetName(), slot, item->GetName() );

    /* Wield: mesh in a slot (no part or texture)
     * Wear: mesh for standalone; texture on a part when worn
     *
     * We'll send the info the client needs, and it figures the rest out.
     */
    if (part.Length() && texture.Length()) 
        mesh.Clear();

    psEquipmentMessage msg( 0, eid, equipped, slot, mesh, part, texture, partMesh );
    CS_ASSERT( msg.valid );
    
    psserver->GetEventManager()->Multicast( msg.msg,
                                            actor->GetMulticastClients(),
                                            0, // Multicast to all without exception
                                            PROX_LIST_ANY_RANGE );
}

void psServerCharManager::SendPlayerMoney( Client *client )
{
    csString buff;

    if (client->GetCharacterData()==NULL)
        return;

    psMoney money=client->GetCharacterData()->Money();

    csString money_str = money.ToString();
    buff.Format("<M MONEY=\"%s\" />",money_str.GetData());

    psGUIMerchantMessage msg(client->GetClientNum(),
                              psGUIMerchantMessage::MONEY,buff);
    psserver->GetEventManager()->SendMessage(msg.msg);
}



bool psServerCharManager::SendItemDescription( Client *client, psItem *item)
{
    if (item==NULL)
        return false;

    unsigned int stackCount = item->GetStackCount();
    csString itemInfo, weight, size, itemQuality, itemCategory, itemName;

    psItemStats* itemStats = item->GetCurrentStats();

    if(item->GetIsLocked())
    {
        itemInfo = "This item is locked\n\n";
    }

    itemCategory.Format( "Category: %s", itemStats->GetCategory()->name.GetData() );
    weight.Format("\nWeight: %.1f", item->GetWeight() );
    size.Format("\nSize: %hu", item->GetItemSize() );

	// If the item is an average stackable type object it has no max quality so don't 
	// send that information to the client since it is not applicable.
	if ( itemStats->GetFlags() & PSITEMSTATS_FLAG_AVERAGEQUALITY )
		itemQuality.Format("\nAverage Quality: %.0f", item->GetItemQuality() );
	else
		itemQuality.Format("\nQuality: %.0f/%.0f", item->GetItemQuality(),item->GetMaxItemQuality() );

    itemInfo += itemCategory+weight+size+itemQuality;
    itemName = item->GetName();

    // Item was crafted
    if ( item->GetCrafterID() != 0 )
    {
        // Add craft adjective
        itemName.Format("%s %s",item->GetQualityString(),item->GetName());

        // Crafter and guild names
        csString crafterInfo;
        psCharacter* charData = psServer::CharacterLoader.QuickLoadCharacterData( item->GetCrafterID(), true );
        if ( charData )
        {
            crafterInfo.Format( "\n\nCrafter: %s", charData->GetCharFullName());
            itemInfo += crafterInfo;
        }
        
        // Item was crafted by a guild member
        if ( item->GetGuildID() != 0 )
        {
            csString guildInfo;
            psGuildInfo* guild = CacheManager::GetSingleton().FindGuild( item->GetGuildID() );
            if ( guild && !guild->IsSecret())
            {
                guildInfo.Format( "\nGuild: %s", guild->GetName().GetData());
                itemInfo += guildInfo;
            }
        }
    }

    if ( item->GetGuardingCharacterID() )
    {
        csString guardingChar;
        gemActor *guardian = GEMSupervisor::GetSingleton().FindPlayerEntity(item->GetGuardingCharacterID());
        if (guardian && guardian->GetCharacterData())
        {
            guardingChar.Format("\nGuarded by: %s", guardian->GetCharacterData()->GetCharFullName());
            itemInfo += guardingChar;
        }
    }

    // Item is a key
    if ( item->GetIsKey() )
    {
        csString lockInfo;
        if (item->GetIsMasterKey())
        {
            lockInfo.Format( "\nThis is a master key!");
            itemInfo += lockInfo;
        }
        lockInfo.Format( "\nOpens: %s", item->GetOpenableLockNames().GetData());
        itemInfo += lockInfo;
    }

    // Item is a weapon
    if ( item->GetCategory()->id == 1 )
    {
        csString speed, damage;
        // Weapon Speed
        speed.Format( "\n\nSpeed: %.2f", itemStats->Weapon().Latency() );
        
        // Weapon Damage Type
        damage = "\n\nDamage:";
        float dmgSlash, dmgBlunt, dmgPierce;
        dmgSlash = item->GetDamage(PSITEMSTATS_DAMAGETYPE_SLASH);
        dmgBlunt = item->GetDamage(PSITEMSTATS_DAMAGETYPE_BLUNT);
        dmgPierce = item->GetDamage(PSITEMSTATS_DAMAGETYPE_PIERCE);

        // Only worth printing if their value is not zero
        if ( dmgSlash )
            damage += csString().Format( "\n Slash: %.2f", dmgSlash );
        if ( dmgBlunt )
            damage += csString().Format( "\n Blunt: %.2f", dmgBlunt );
        if ( dmgPierce )
            damage += csString().Format( "\n Pierce: %.2f", dmgPierce );

        itemInfo+= speed + damage;
    }

    // Item is armor
    if ( item->GetCategory()->id == 2)
    {
        csString armor_type = "\n\n";
        switch (item->GetBaseStats()->Armor().Type())
        {
            case PSITEMSTATS_ARMORTYPE_LIGHT:
                armor_type.Append("Light Armor");
                break;
            case PSITEMSTATS_ARMORTYPE_MEDIUM:
                armor_type.Append("Medium Armor");
                break;
            case PSITEMSTATS_ARMORTYPE_HEAVY:
                armor_type.Append("Heavy Armor");
                break;
	    default:
	        break;
        }
        itemInfo += armor_type;
    }

    if (strcmp(item->GetDescription(),"0") != 0)
    {
        itemInfo += "\n\nDescription: ";
        itemInfo += item->GetDescription();
    }
   
    psViewItemDescription outgoing( client->GetClientNum(), itemName.GetData(), itemInfo.GetData(), item->GetImageName(), stackCount );

    if ( outgoing.valid )
        psserver->GetEventManager()->SendMessage(outgoing.msg);
    else
    {
        Bug2("Could not create valid psViewItemDescription for client %u.\n",client->GetClientNum());
        return false;
    }

    return true;
}


bool psServerCharManager::SendContainerContents(Client *client, psItem *item, int containerID)
{
    if (item==NULL)
        return false;
    
    if (item->GetIsLocked())
        return SendItemDescription(client,item);

    csString name( item->GetName() );
    csString icon( item->GetImageName() );
    csString desc( item->GetDescription() );
	
    // FIXME: This function is called for world containers too...
    desc.AppendFmt("\n\nWeight: %.2f\nCapacity: %u/%u",
                    client->GetCharacterData()->Inventory().GetContainedWeight(item), 
                    client->GetCharacterData()->Inventory().GetContainedSize(item), 
                    item->GetContainerMaxSize() );

    psViewItemDescription outgoing( client->GetClientNum(),
                                    name,
                                    desc,
                                    icon,
                                    0,
                                    IS_CONTAINER );
                                          
    if (item->GetGemObject() != NULL )
        outgoing.containerID = item->GetGemObject()->GetEntity()->GetID();
    else
        outgoing.containerID = containerID;        

    FillContainerMsg( item, client, outgoing );
    
    outgoing.ConstructMsg();
    outgoing.SendMessage();

    return true;
}


bool psServerCharManager::SendActionContents(Client *client, psActionLocation *action,
                                             psItem *item)
{
    if ( action == NULL )
        return false;

    if ( item == NULL )
        return false;
    
    //if(item->GetIsLocked())
    //    return SendItemDescription(client,item);

    csString name( action->name );
    csString desc( item->GetDescription() );
    csString icon( item->GetImageName() );

    csString description = action->GetDescription();
    if ( description )
    {
        desc = description.GetData();
    }

    bool isContainer = item->GetIsContainer();
	
    psViewItemDescription outgoing( client->GetClientNum(),
                                    name,
                                    desc,
                                    icon,
                                    0,
                                    isContainer );

    /* REMOVED: was probably there to avoid a crash, remove after some testing.
    if (action->GetGemObject() != NULL )
        outgoing.containerID = action->GetGemObject()->GetEntity()->GetID();
    else
        outgoing.containerID = containerID;
    */

    outgoing.containerID = item->GetGemObject()->GetEntity()->GetID();

    if ( isContainer )
    {           
        FillContainerMsg( item, client, outgoing );
        outgoing.ConstructMsg();    
    }
    
    outgoing.SendMessage();
    return true;
}


void psServerCharManager::FillContainerMsg( psItem* item, Client* client, psViewItemDescription& outgoing )
{
    gemContainer *container = dynamic_cast<gemContainer*> (item->GetGemObject());
    
    if (!container)
    {
        // This is not a container in the world so it must be a container inside the pserson's
        // inventory.  So check to see which items the player has that are in the container.
        int slot = 0;
        for (size_t i=0; i < client->GetCharacterData()->Inventory().GetInventoryIndexCount(); i++)
        {
            psItem *child = client->GetCharacterData()->Inventory().GetInventoryIndexItem(i);
            if (item->GetContainerID() == item->GetUID())
            {
                outgoing.AddContents( child->GetName(),  child->GetImageName(), child->GetPurifyStatus(), slot++, child->GetStackCount());
            }
        }
        return;
    }

    gemContainer::psContainerIterator it(container);
    while (it.HasNext())
    {
        psItem* child = it.Next();
        if (!child)
        {
            Debug2(LOG_NET,client->GetClientNum(),"Container iterator has next but returns null psItem pointer for %u.\n",client->GetClientNum());
            return;
        }

        if (client->CanTake(child))
        {
            outgoing.AddContents(child->GetName(), child->GetImageName(), child->GetPurifyStatus(), 
                                 child->GetLocInParent(), child->GetStackCount());
        }
        else
        {
            //const char *icon = "/this/art/gui/slottaken_icon.png" ;
            outgoing.AddContents(child->GetName(), child->GetImageName(), child->GetPurifyStatus(),
                                 child->GetLocInParent(), -1);
        }
    }
}


bool psServerCharManager::SendBookText(Client *client, psItem *item, int containerID, int slotID)
{
    if (item==NULL)
        return false;

    csString name = item->GetName();
    csString text;
    csString lockedText("This item is locked\n");
    if(item->GetIsLocked())
    {
        text = lockedText;
    } else text = item->GetBookText();
    //determine whether to display the 'write' button
    //and send the appropriate information if so

    bool shouldWrite = item->GetBaseStats()->GetIsWriteable();
  //  CPrintf(CON_DEBUG,"Sent text for book %u %u\n",slotID, containerID);
    psReadBookTextMessage outgoing(client->GetClientNum(), name, text, shouldWrite, slotID, containerID);

    if (outgoing.valid)
    {
        psserver->GetEventManager()->SendMessage(outgoing.msg);
    }
    else
    {
        Bug2("Could not create valid psReadBookText for client %u.\n",client->GetClientNum());
        return false;
    }

    return true;
}


bool psServerCharManager::SendMerchantItems( Client *client, psCharacter* merchant, psItemCategory* category)
{
    csArray<psItem*> items = merchant->Inventory().GetItemsInCategory(category);

    // Build item list
    csString buff("<L>");
    csString item;

    for ( size_t z = 0; z < items.GetSize(); z++ )
    {
        if (items[z]->IsEquipped())
            continue;

        csString escpxml_name = EscpXML(items[z]->GetName());
        csString escpxml_imagename = EscpXML(items[z]->GetImageName());
        item.Format("<ITEM ID=\"%u\" "
                    "NAME=\"%s\" "
                    "IMG=\"%s\" "
                    "PRICE=\"%i\" "
                    "COUNT=\"%i\" />",
                    items[z]->GetUID(),
                    escpxml_name.GetData(),
                    escpxml_imagename.GetData(),
                    CalculateMerchantPrice(items[z], client, false),
                    items[z]->GetStackCount());

        buff.Append(item);
    }
    buff.Append("</L>");

    psGUIMerchantMessage msg4(client->GetClientNum(),psGUIMerchantMessage::ITEMS,buff.GetData());
    if (msg4.valid)
        psserver->GetEventManager()->SendMessage(msg4.msg);
    else
    {
        Bug2("Could not create valid psGUIMerchantMessage for client %u.\n",client->GetClientNum());
    }


    return true;
}

int psServerCharManager::CalculateMerchantPrice(psItem *item, Client *client, bool sellPrice)
{
    int basePrice = sellPrice?item->GetSellPrice().GetTotal():item->GetPrice().GetTotal();
    int finalPrice = basePrice;
    if((sellPrice && !calc_item_merchant_price_sell) || (!sellPrice && !calc_item_merchant_price_buy))
        return basePrice;

    if (sellPrice)
    {
        calc_item_merchant_price_char_data_sell->SetObject(client->GetCharacterData());
        calc_item_merchant_price_item_price_sell->SetValue(basePrice);
        calc_item_merchant_price_sell->Execute();
        finalPrice = calc_item_merchant_price_char_result_sell->GetValue();
    }
    else
    {
        calc_item_merchant_price_char_data_buy->SetObject(client->GetCharacterData());
        calc_item_merchant_price_item_price_buy->SetValue(basePrice);
        calc_item_merchant_price_buy->Execute();
        finalPrice = calc_item_merchant_price_char_result_buy->GetValue();
    }
    return finalPrice;
}

bool psServerCharManager::SendPlayerItems( Client *client, psItemCategory* category)
{
    csArray<psItem*> items = client->GetCharacterData()->Inventory().GetItemsInCategory(category);

    // Build item list
    csString buff("<L>");
    csString item;
    csString itemID;

    for ( size_t z = 0; z < items.GetSize(); z++ )
    {
        if (items[z]->IsEquipped())
            continue;

        itemID.Format("%u",items[z]->GetUID());
        csString purified;

        if (items[z]->GetPurifyStatus() == 2)
            purified = "yes";
        else
            purified = "no";

        csString escpxml_name = EscpXML(items[z]->GetName());
        csString escpxml_imagename = EscpXML(items[z]->GetImageName());
        item.Format("<ITEM ID=\"%s\" "
                    "NAME=\"%s\" "
                    "IMG=\"%s\" "
                    "PRICE=\"%i\" "
                    "COUNT=\"%i\" " 
                    "PURIFIED=\"%s\" />",
                    itemID.GetDataSafe(),
                    escpxml_name.GetData(),
                    escpxml_imagename.GetData(),
                    CalculateMerchantPrice(items[z], client, true),
                    items[z]->GetStackCount(),
                    purified.GetData());

        buff.Append(item);
    }
    buff.Append("</L>");

    psGUIMerchantMessage msg4(client->GetClientNum(),psGUIMerchantMessage::ITEMS,buff.GetData());
    if (msg4.valid)
        psserver->GetEventManager()->SendMessage(msg4.msg);
    else
    {
        Bug2("Could not create valid psGUIMerchantMessage for client %u.\n",client->GetClientNum());
    }


    return true;
}

bool psServerCharManager::VerifyGoal(Client* client, psCharacter* character, psItem* goal)
{
    // glyph items can't be goals
    if (goal->GetCurrentStats()->GetIsGlyph())
        return false;

    return true;
}

