/*
 * events.h
 *
 * Copyright (C) 2004 Atomic Blue (info@planeshift.it, http://www.atomicblue.org) 
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
 * Author: Keith Fulton <keith@planeshift.it>
 */

#ifndef EVENTS_H_Z
#define EVENTS_H_Z

#include <net/messages.h>


struct TransactionEntity;
class gemActor;
class gemObject;

class psDamageEvent : public psMessageCracker
{
public:
    gemActor *attacker;
    gemActor *target;
    float     damage;

    psDamageEvent(gemActor *attack,gemActor *victim,float dmg);
    psDamageEvent( MsgEntry* event );

    PSF_DECLARE_MSG_FACTORY();

    /**
     * Convert the message into human readable string.
     *
     * @param access_ptrs A struct to a number of access pointers.
     * @return Return a human readable string for the message.
     */
    virtual csString ToString(AccessPointers * access_ptrs);
};

class psDeathEvent : public psMessageCracker
{
public:
    gemActor *deadActor;
    gemActor *killer;

    psDeathEvent(gemActor *dead, gemActor *killer);
    psDeathEvent( MsgEntry* event );

    PSF_DECLARE_MSG_FACTORY();

    /**
     * Convert the message into human readable string.
     *
     * @param access_ptrs A struct to a number of access pointers.
     * @return Return a human readable string for the message.
     */
    virtual csString ToString(AccessPointers * access_ptrs);
};

class psTargetChangeEvent : public psMessageCracker
{
 public:
    gemActor *character;
    gemObject *target;

    psTargetChangeEvent(gemActor *targeter, gemObject *targeted);
    psTargetChangeEvent(MsgEntry *event);

    PSF_DECLARE_MSG_FACTORY();

    /**
     * Convert the message into human readable string.
     *
     * @param access_ptrs A struct to a number of access pointers.
     * @return Return a human readable string for the message.
     */
    virtual csString ToString(AccessPointers * access_ptrs);
};

/** Event when a player gains some Z points.
 */
class psZPointsGainedEvent : public psMessageCracker
{
public:
    psZPointsGainedEvent( gemActor* actor, const char* name, int gained, bool rankup );
    psZPointsGainedEvent( MsgEntry *event );

    PSF_DECLARE_MSG_FACTORY();

    /**
     * Convert the message into human readable string.
     *
     * @param access_ptrs A struct to a number of access pointers.
     * @return Return a human readable string for the message.
     */
    virtual csString ToString(AccessPointers * access_ptrs);
    
public:
    gemActor* actor;        /// The player that gained the points.
    int amountGained;       /// The amount that was gained.
    bool rankUp;            /// True if the amount gained caused a rank up
    csString skillName;     /// The name of the skill points gained in.        
};

class psBuyEvent : public psMessageCracker
{
public:
    psBuyEvent( int from, int to, unsigned int item, int stack, int quality,unsigned int price);
    psBuyEvent( MsgEntry* event);

    PSF_DECLARE_MSG_FACTORY();

    /**
     * Convert the message into human readable string.
     *
     * @param access_ptrs A struct to a number of access pointers.
     * @return Return a human readable string for the message.
     */
    virtual csString ToString(AccessPointers * access_ptrs);

    TransactionEntity* trans;
};

class psSellEvent : public psMessageCracker
{
public:
    psSellEvent( int from, int to, unsigned int item, int stack, int quality,unsigned int price);
    psSellEvent( MsgEntry* event);

    PSF_DECLARE_MSG_FACTORY();

    /**
     * Convert the message into human readable string.
     *
     * @param access_ptrs A struct to a number of access pointers.
     * @return Return a human readable string for the message.
     */
    virtual csString ToString(AccessPointers * access_ptrs);

    TransactionEntity* trans;
};

/**
 * This message broadcasts client connect events to anyone
 * who needs them, namely the TutorialManager.
 */
class psConnectEvent : public psMessageCracker
{
protected:
    int client_id;

public:
    psConnectEvent( int clientID );
    psConnectEvent( MsgEntry* event);

    PSF_DECLARE_MSG_FACTORY();

    /**
     * Convert the message into human readable string.
     *
     * @param access_ptrs A struct to a number of access pointers.
     * @return Return a human readable string for the message.
     */
    virtual csString ToString(AccessPointers * access_ptrs);
};

/**
 * This message broadcasts the first movement event to anyone
 * who needs them, namely the TutorialManager.
 */
class psMovementEvent : public psMessageCracker
{
protected:
    int client_id;

public:
    psMovementEvent( int clientID );
    psMovementEvent( MsgEntry* event);

    PSF_DECLARE_MSG_FACTORY();

    /**
     * Convert the message into human readable string.
     *
     * @param access_ptrs A struct to a number of access pointers.
     * @return Return a human readable string for the message.
     */
    virtual csString ToString(AccessPointers * access_ptrs);
};


/**
 * This message broadcasts several different events to anyone
 * who needs them, namely the TutorialManager.
 */
class psGenericEvent : public psMessageCracker
{
public:
    enum Type
    {
        UNKNOWN=0,
        QUEST_ASSIGN
    };

    Type eventType;
    int client_id;
    

    psGenericEvent( int clientID, psGenericEvent::Type type );
    psGenericEvent( MsgEntry* event);

    PSF_DECLARE_MSG_FACTORY();

    /**
     * Convert the message into human readable string.
     *
     * @param access_ptrs A struct to a number of access pointers.
     * @return Return a human readable string for the message.
     */
    virtual csString ToString(AccessPointers * access_ptrs);

};

#endif
