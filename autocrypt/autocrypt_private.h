/**
 * @file
 * XXX
 *
 * @authors
 * Copyright (C) 2019 Kevin J. McCarthy <kevin@8t8.us>
 *
 * @copyright
 * This program is free software: you can redistribute it and/or modify it under
 * the terms of the GNU General Public License as published by the Free Software
 * Foundation, either version 2 of the License, or (at your option) any later
 * version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
 * details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef MUTT_AUTOCRYPT_AUTOCRYPT_PRIVATE_H
#define MUTT_AUTOCRYPT_AUTOCRYPT_PRIVATE_H

#include <sqlite3.h>

struct Address;
struct AddressList;
struct Buffer;

int mutt_autocrypt_account_init (int prompt);

int mutt_autocrypt_db_init (int can_create);
void mutt_autocrypt_db_close (void);

void mutt_autocrypt_db_normalize_addr(struct Address *a);
void mutt_autocrypt_db_normalize_addrlist(struct AddressList *al);

struct AutocryptAccount *mutt_autocrypt_db_account_new (void);
void mutt_autocrypt_db_account_free (struct AutocryptAccount **account);
int mutt_autocrypt_db_account_get (struct Address *addr, struct AutocryptAccount **account);
int mutt_autocrypt_db_account_insert (struct Address *addr, const char *keyid,
                                      const char *keydata, int prefer_encrypt);
int mutt_autocrypt_db_account_update (struct AutocryptAccount *acct);
int mutt_autocrypt_db_account_delete (struct AutocryptAccount *acct);
int mutt_autocrypt_db_account_get_all (struct AutocryptAccount ***accounts, int *num_accounts);

struct AutocryptPeer *mutt_autocrypt_db_peer_new (void);
void mutt_autocrypt_db_peer_free (struct AutocryptPeer **peer);
int mutt_autocrypt_db_peer_get (struct Address *addr, struct AutocryptPeer **peer);
int mutt_autocrypt_db_peer_insert (struct Address *addr, struct AutocryptPeer *peer);
int mutt_autocrypt_db_peer_update (struct AutocryptPeer *peer);

struct AutocryptPeerHistory *mutt_autocrypt_db_peer_history_new (void);
void mutt_autocrypt_db_peer_history_free (struct AutocryptPeerHistory **peerhist);
int mutt_autocrypt_db_peer_history_insert (struct Address *addr, struct AutocryptPeerHistory *peerhist);

struct AutocryptGossipHistory *mutt_autocrypt_db_gossip_history_new (void);
void mutt_autocrypt_db_gossip_history_free (struct AutocryptGossipHistory **gossip_hist);
int mutt_autocrypt_db_gossip_history_insert (struct Address *addr, struct AutocryptGossipHistory *gossip_hist);

int mutt_autocrypt_schema_init (void);
int mutt_autocrypt_schema_update (void);

int mutt_autocrypt_gpgme_init (void);
int mutt_autocrypt_gpgme_create_key (struct Address *addr, struct Buffer *keyid, struct Buffer *keydata);
int mutt_autocrypt_gpgme_import_key (const char *keydata, struct Buffer *keyid);
int mutt_autocrypt_gpgme_is_valid_key (const char *keyid);

#endif /* MUTT_AUTOCRYPT_AUTOCRYPT_PRIVATE_H */
