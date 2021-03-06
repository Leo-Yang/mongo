/**
 *    Copyright (C) 2008-2015 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kSharding

#include "mongo/platform/basic.h"

#include "mongo/s/client/shard.h"

#include <boost/make_shared.hpp>
#include <set>
#include <string>
#include <vector>

#include "mongo/client/connpool.h"
#include "mongo/client/dbclientcursor.h"
#include "mongo/client/replica_set_monitor.h"
#include "mongo/db/auth/action_set.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/commands.h"
#include "mongo/db/jsobj.h"
#include "mongo/s/catalog/catalog_manager.h"
#include "mongo/s/catalog/type_shard.h"
#include "mongo/s/grid.h"
#include "mongo/util/log.h"

namespace mongo {

    using std::list;
    using std::map;
    using std::ostream;
    using std::string;
    using std::stringstream;
    using std::vector;


    class StaticShardInfo {
    public:
        void reload() {

            vector<ShardType> shards;
            Status status = grid.catalogManager()->getAllShards(&shards);
            massert(13632, "couldn't get updated shard list from config server", status.isOK());

            int numShards = shards.size();

            LOG(1) << "found " << numShards << " shards listed on config server(s)";

            boost::lock_guard<boost::mutex> lk( _mutex );

            // We use the _lookup table for all shards and for the primary config DB. The config DB info,
            // however, does not come from the ShardNS::shard. So when cleaning the _lookup table we leave
            // the config state intact. The rationale is that this way we could drop shards that
            // were removed without reinitializing the config DB information.

            ShardMap::iterator i = _lookup.find( "config" );
            if ( i != _lookup.end() ) {
                ShardPtr config = i->second;
                _lookup.clear();
                _lookup[ "config" ] = config;
            }
            else {
                _lookup.clear();
            }
            _rsLookup.clear();
            
            for (const ShardType& shardData : shards) {
                uassertStatusOK(shardData.validate());

                ShardPtr shard = boost::make_shared<Shard>(shardData.getName(),
                                                           shardData.getHost(),
                                                           shardData.getMaxSize(),
                                                           shardData.getDraining());

                _lookup[shardData.getName()] = shard;
                _installHost(shardData.getHost(), shard);
            }

        }

        ShardPtr findUsingLookUp(const string& shardName) {
            ShardMap::iterator it;
            {
                boost::lock_guard<boost::mutex> lk(_mutex);
                it = _lookup.find(shardName);
            }
            if (it != _lookup.end()) return it->second;
            return ShardPtr();
        }

        ShardPtr findIfExists(const string& shardName) {
            ShardPtr shard = findUsingLookUp(shardName);
            if (shard) {
                return shard;
            }
            // if we can't find the shard, we might just need to reload the cache
            reload();
            return findUsingLookUp(shardName);
        }

        ShardPtr find(const string& ident) {
            string errmsg;
            ConnectionString connStr = ConnectionString::parse(ident, errmsg);

            uassert(18642, str::stream() << "Error parsing connection string: " << ident,
                    errmsg.empty());

            if (connStr.type() == ConnectionString::SET) {
                boost::lock_guard<boost::mutex> lk(_rsMutex);
                ShardMap::iterator iter = _rsLookup.find(connStr.getSetName());

                if (iter == _rsLookup.end()) {
                    return ShardPtr();
                }

                return iter->second;
            }
            else {
                boost::lock_guard<boost::mutex> lk(_mutex);
                ShardMap::iterator iter = _lookup.find(ident);

                if (iter == _lookup.end()) {
                    return ShardPtr();
                }

                return iter->second;
            }
        }

        ShardPtr findWithRetry(const string& ident) {
            ShardPtr shard(find(ident));

            if (shard != NULL) {
                return shard;
            }

            // not in our maps, re-load all
            reload();

            shard = find(ident);
            massert(13129 , str::stream() << "can't find shard for: " << ident, shard != NULL);
            return shard;
        }

        // Lookup shard by replica set name. Returns Shard::EMTPY if the name can't be found.
        // Note: this doesn't refresh the table if the name isn't found, so it's possible that
        // a newly added shard/Replica Set may not be found.
        Shard lookupRSName( const string& name) {
            boost::lock_guard<boost::mutex> lk( _rsMutex );
            ShardMap::iterator i = _rsLookup.find( name );

            return (i == _rsLookup.end()) ? Shard::EMPTY : *(i->second.get());
        }

        // Useful for ensuring our shard data will not be modified while we use it
        Shard findCopy( const string& ident ){
            ShardPtr found = findWithRetry(ident);
            boost::lock_guard<boost::mutex> lk( _mutex );
            massert( 13128 , (string)"can't find shard for: " + ident , found.get() );
            return *found.get();
        }

        void set( const string& name , const Shard& s , bool setName = true , bool setAddr = true ) {
            boost::lock_guard<boost::mutex> lk( _mutex );
            ShardPtr ss( new Shard( s ) );
            if ( setName )
                _lookup[name] = ss;
            if ( setAddr )
                _installHost( s.getConnString() , ss );
        }

        void _installHost( const string& host , const ShardPtr& s ) {
            _lookup[host] = s;

            const ConnectionString& cs = s->getAddress();
            if ( cs.type() == ConnectionString::SET ) {
                if ( cs.getSetName().size() ) {
                    boost::lock_guard<boost::mutex> lk( _rsMutex);
                    _rsLookup[ cs.getSetName() ] = s;
                }
                vector<HostAndPort> servers = cs.getServers();
                for ( unsigned i=0; i<servers.size(); i++ ) {
                    _lookup[ servers[i].toString() ] = s;
                }
            }
        }

        void remove( const string& name ) {
            boost::lock_guard<boost::mutex> lk( _mutex );
            for ( ShardMap::iterator i = _lookup.begin(); i!=_lookup.end(); ) {
                ShardPtr s = i->second;
                if ( s->getName() == name ) {
                    _lookup.erase(i++);
                }
                else {
                    ++i;
                }
            }
            for ( ShardMap::iterator i = _rsLookup.begin(); i!=_rsLookup.end(); ) {
                ShardPtr s = i->second;
                if ( s->getName() == name ) {
                    _rsLookup.erase(i++);
                }
                else {
                    ++i;
                }
            }
        }

        void getAllShards( vector<ShardPtr>& all ) const {
            boost::lock_guard<boost::mutex> lk( _mutex );
            std::set<string> seen;
            for ( ShardMap::const_iterator i = _lookup.begin(); i!=_lookup.end(); ++i ) {
                const ShardPtr& s = i->second;
                if ( s->getName() == "config" )
                    continue;
                if ( seen.count( s->getName() ) )
                    continue;
                seen.insert( s->getName() );
                all.push_back( s );
            }
        }

        void getAllShards( vector<Shard>& all ) const {
            boost::lock_guard<boost::mutex> lk( _mutex );
            std::set<string> seen;
            for ( ShardMap::const_iterator i = _lookup.begin(); i!=_lookup.end(); ++i ) {
                const ShardPtr& s = i->second;
                if ( s->getName() == "config" )
                    continue;
                if ( seen.count( s->getName() ) )
                    continue;
                seen.insert( s->getName() );
                all.push_back( *s );
            }
        }


        bool isAShardNode( const string& addr ) const {
            boost::lock_guard<boost::mutex> lk( _mutex );

            // check direct nods or set names
            ShardMap::const_iterator i = _lookup.find( addr );
            if ( i != _lookup.end() )
                return true;

            // check for set nodes
            for ( ShardMap::const_iterator i = _lookup.begin(); i!=_lookup.end(); ++i ) {
                if ( i->first == "config" )
                    continue;

                if ( i->second->containsNode( addr ) )
                    return true;
            }

            return false;
        }

        bool getShardMap( BSONObjBuilder& result , string& errmsg ) const {
            boost::lock_guard<boost::mutex> lk( _mutex );

            BSONObjBuilder b( _lookup.size() + 50 );

            for ( ShardMap::const_iterator i = _lookup.begin(); i!=_lookup.end(); ++i ) {
                b.append( i->first , i->second->getConnString() );
            }

            result.append( "map" , b.obj() );

            return true;
        }

    private:
        typedef map<string,ShardPtr> ShardMap;
        ShardMap _lookup; // Map of both shardName -> Shard and hostName -> Shard
        ShardMap _rsLookup; // Map from ReplSet name to shard
        mutable mongo::mutex _mutex;
        mutable mongo::mutex _rsMutex;
    } staticShardInfo;


    class CmdGetShardMap : public Command {
    public:
        CmdGetShardMap() : Command( "getShardMap" ){}
        virtual void help( stringstream &help ) const { help<<"internal"; }
        virtual bool isWriteCommandForConfigServer() const { return false; }
        virtual bool slaveOk() const { return true; }
        virtual bool adminOnly() const { return true; }
        virtual void addRequiredPrivileges(const std::string& dbname,
                                           const BSONObj& cmdObj,
                                           std::vector<Privilege>* out) {
            ActionSet actions;
            actions.addAction(ActionType::getShardMap);
            out->push_back(Privilege(ResourcePattern::forClusterResource(), actions));
        }
        virtual bool run(OperationContext* txn,
                         const string&,
                         mongo::BSONObj&,
                         int,
                         std::string& errmsg ,
                         mongo::BSONObjBuilder& result) {
            return staticShardInfo.getShardMap( result , errmsg );
        }
    } cmdGetShardMap;

    Shard::Shard()
        : _name(""),
          _addr(""),
          _maxSizeMB(0),
          _isDraining(false) {

    }

    Shard::Shard(const std::string& name,
                 const std::string& addr,
                 long long maxSizeMB,
                 bool isDraining)
        : _name(name),
          _addr(addr),
          _maxSizeMB(maxSizeMB),
          _isDraining(isDraining) {

        _setAddr(addr);
    }

    Shard::Shard(const std::string& name,
                 const ConnectionString& connStr,
                 long long maxSizeMB,
                 bool isDraining)
        : _name(name),
          _addr(connStr.toString()),
          _cs(connStr),
          _maxSizeMB(maxSizeMB),
          _isDraining(isDraining) {

    }

    Shard Shard::findIfExists( const string& shardName ) {
        ShardPtr shard = staticShardInfo.findIfExists( shardName );
        return shard ? *shard : Shard::EMPTY;
    }

    void Shard::_setAddr( const string& addr ) {
        _addr = addr;
        if ( !_addr.empty() ) {
            _cs = ConnectionString( addr , ConnectionString::SET );
        }
    }

    void Shard::reset( const string& ident ) {
        *this = staticShardInfo.findCopy( ident );
    }

    bool Shard::containsNode( const string& node ) const {
        if ( _addr == node )
            return true;

        if ( _cs.type() == ConnectionString::SET ) {
            ReplicaSetMonitorPtr rs = ReplicaSetMonitor::get( _cs.getSetName(), true );

            if (!rs) {
                // Possibly still yet to be initialized. See SERVER-8194.
                warning() << "Monitor not found for a known shard: " << _cs.getSetName();
                return false;
            }

            return rs->contains(HostAndPort(node));
        }

        return false;
    }

    void Shard::getAllShards( vector<Shard>& all ) {
        staticShardInfo.getAllShards( all );
    }

    bool Shard::isAShardNode( const string& ident ) {
        return staticShardInfo.isAShardNode( ident );
    }

    Shard Shard::lookupRSName( const string& name) {
        return staticShardInfo.lookupRSName(name);
    }

    void Shard::printShardInfo( ostream& out ) {
        vector<Shard> all;
        staticShardInfo.getAllShards( all );
        for ( unsigned i=0; i<all.size(); i++ )
            out << all[i].toString() << "\n";
        out.flush();
    }

    BSONObj Shard::runCommand( const string& db , const BSONObj& cmd ) const {
        BSONObj res;
        bool ok = runCommand(db, cmd, res);
        if ( ! ok ) {
            stringstream ss;
            ss << "runCommand (" << cmd << ") on shard (" << _name << ") failed : " << res;
            throw UserException( 13136 , ss.str() );
        }
        res = res.getOwned();
        return res;
    }

    bool Shard::runCommand(const string& db,
                           const BSONObj& cmd,
                           BSONObj& res) const {
        ScopedDbConnection conn(getConnString());
        bool ok = conn->runCommand(db, cmd, res);
        conn.done();
        return ok;
    }

    string Shard::getShardMongoVersion(const string& shardHost) {
        ScopedDbConnection conn(shardHost);
        BSONObj serverStatus;
        bool ok = conn->runCommand("admin", BSON("serverStatus" << 1), serverStatus);
        conn.done();

        uassert(28598,
                str::stream() << "call to serverStatus on " << shardHost
                              << " failed: " << serverStatus,
                ok);

        BSONElement versionElement = serverStatus["version"];

        uassert(28589, "version field not found in serverStatus",
                versionElement.type() == String);
        return serverStatus["version"].String();
    }

    long long Shard::getShardDataSizeBytes(const string& shardHost) {
        ScopedDbConnection conn(shardHost);
        BSONObj listDatabases;
        bool ok = conn->runCommand("admin", BSON("listDatabases" << 1), listDatabases);
        conn.done();

        uassert(28599,
                str::stream() << "call to listDatabases on " << shardHost
                              << " failed: " << listDatabases,
                ok);

        BSONElement totalSizeElem = listDatabases["totalSize"];

        uassert(28590, "totalSize field not found in listDatabases",
                totalSizeElem.isNumber());
        return listDatabases["totalSize"].numberLong();
    }

    ShardStatus Shard::getStatus() const {
        return ShardStatus(*this,
                           getShardDataSizeBytes(getConnString()),
                           getShardMongoVersion(getConnString()));
    }

    void Shard::reloadShardInfo() {
        staticShardInfo.reload();
    }


    void Shard::removeShard( const string& name ) {
        staticShardInfo.remove( name );
    }

    Shard Shard::pick( const Shard& current ) {
        vector<Shard> all;
        staticShardInfo.getAllShards( all );
        if ( all.size() == 0 ) {
            staticShardInfo.reload();
            staticShardInfo.getAllShards( all );
            if ( all.size() == 0 )
                return EMPTY;
        }

        // if current shard was provided, pick a different shard only if it is a better choice
        ShardStatus best = all[0].getStatus();
        if ( current != EMPTY ) {
            best = current.getStatus();
        }

        for ( size_t i=0; i<all.size(); i++ ) {
            ShardStatus t = all[i].getStatus();
            if ( t < best )
                best = t;
        }

        LOG(1) << "best shard for new allocation is " << best;
        return best.shard();
    }

    void Shard::installShard(const std::string& name, const Shard& shard) {
        staticShardInfo.set(name, shard, true, false);
    }

    ShardStatus::ShardStatus(const Shard& shard, long long dataSizeBytes, const string& version):
            _shard(shard), _dataSizeBytes(dataSizeBytes), _mongoVersion(version) {
    }

} // namespace mongo
