// Copyright (c) YugaByte, Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may not use this file except
// in compliance with the License.  You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software distributed under the License
// is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express
// or implied.  See the License for the specific language governing permissions and limitations
// under the License.
//

package org.yb.pgsql;

import org.hamcrest.CoreMatchers;
import org.junit.Before;
import org.junit.Test;
import org.junit.runner.RunWith;
import org.slf4j.Logger;
import org.slf4j.LoggerFactory;
import org.yb.pgsql.cleaners.ClusterCleaner;
import org.yb.pgsql.cleaners.RoleCleaner;
import org.yb.pgsql.cleaners.TabletServerCleaner;
import org.yb.util.YBTestRunnerNonTsanOnly;

import java.sql.Connection;
import java.sql.SQLException;
import java.sql.Statement;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;
import java.util.TreeMap;

import static org.yb.AssertionWrappers.assertThat;
import static org.yb.AssertionWrappers.fail;
import static org.yb.pgsql.ComparableArray.arrayOf;

/**
 * Tests for PostgreSQL configuration.
 */
@RunWith(value = YBTestRunnerNonTsanOnly.class)
public class TestPgConfiguration extends BasePgSQLTest {
  private static final Logger LOG = LoggerFactory.getLogger(TestPgConfiguration.class);

  private TabletServerCleaner tabletServerCleaner;

  @Override
  protected TreeMap<Integer, ClusterCleaner> getCleaners() {
    TreeMap<Integer, ClusterCleaner> cleaners = super.getCleaners();
    tabletServerCleaner = new TabletServerCleaner();
    cleaners.put(0, tabletServerCleaner);
    cleaners.put(1, new RoleCleaner());
    return cleaners;
  }

  @Before
  public void before() {
    tabletServerCleaner.snapshot(miniCluster);
  }

  @Test
  public void testPostgresConfigDefault() throws Exception {
    int tserver = spawnTServerWithFlags();

    try (Connection connection = newConnectionBuilder().setTServer(tserver).connect();
         Statement statement = connection.createStatement()) {
      // Default value determined by local initdb.
      assertQuery(
          statement,
          "SELECT setting, source FROM pg_settings WHERE name='max_connections'",
          new Row("300", "configuration file")
      );

      // Default value determined by the GUC.
      assertQuery(
          statement,
          "SELECT setting, source FROM pg_settings WHERE name='checkpoint_timeout'",
          new Row("300", "default")
      );
    }
  }

  @Test
  public void testPostgresConfigCatchAll() throws Exception {
    int tserver = spawnTServerWithFlags(
        "--ysql_pg_conf=max_connections=46, bonjour_name = 'some name', port=5432");

    try (Connection connection = newConnectionBuilder().setTServer(tserver).connect();
         Statement statement = connection.createStatement()) {
      // Parameters set via gflag.
      assertQuery(
          statement,
          "SELECT setting, source FROM pg_settings WHERE name='max_connections'",
          new Row("46", "configuration file")
      );
      assertQuery(
          statement,
          "SELECT setting, source FROM pg_settings WHERE name='bonjour_name'",
          new Row("some name", "configuration file")
      );

      // Port change is overridden by the tablet server.
      assertQuery(
          statement,
          "SELECT setting, source FROM pg_settings WHERE name='port'",
          new Row(String.valueOf(getPgPort(tserver)), "command line")
      );

      // Default value determined by local initdb.
      assertQuery(
          statement,
          "SELECT setting, source FROM pg_settings WHERE name='default_text_search_config'",
          new Row("pg_catalog.english", "configuration file")
      );
    }
  }

  @Test
  public void testPgHbaConfigDefault() throws Exception {
    try (Statement statement = connection.createStatement()) {
      statement.execute("CREATE ROLE test_role LOGIN PASSWORD 'pass'");
    }

    int tserver = spawnTServerWithFlags();

    // Can connect as test_role.
    try (Connection ignored = newConnectionBuilder().setTServer(tserver)
        .setUser("test_role").connect()) {
      // No-op.
    }

    // Can connect as superuser.
    try (Connection ignored = newConnectionBuilder().setTServer(tserver)
        .setUser("yugabyte").connect()) {
      // No-op.
    }
  }

  @Test
  public void testPgHbaConfigCatchAll() throws Exception {
    try (Statement statement = connection.createStatement()) {
      statement.execute("CREATE ROLE test_role LOGIN PASSWORD 'pass'");
    }

    int tserver = spawnTServerWithFlags("--ysql_hba_conf=" +
        "host all test_role 0.0.0.0/0 password," +
        "host all all 0.0.0.0/0 trust");

    // Can connect as test_role with password.
    try (Connection ignored = newConnectionBuilder().setTServer(tserver)
        .setUser("test_role").setPassword("pass").connect()) {
      // No-op.
    }

    // Can connect as other users without password.
    try (Connection ignored = newConnectionBuilder().setTServer(tserver)
        .setUser("yugabyte").connect()) {
      // No-op.
    }

    // Cannot connect as test_role without password.
    try (Connection ignored = newConnectionBuilder().setTServer(tserver)
        .setUser("test_role").connect()) {
      fail("Expected login attempt to fail");
    } catch (SQLException sqle) {
      assertThat(
          sqle.getMessage(),
          CoreMatchers.containsString("no password was provided")
      );
    }
  }

  @Test
  public void testPgHbaConfigCatchAllReversed() throws Exception {
    try (Statement statement = connection.createStatement()) {
      statement.execute("CREATE ROLE test_role LOGIN PASSWORD 'pass'");
    }

    int tserver = spawnTServerWithFlags("--ysql_hba_conf=" +
        "host all all 0.0.0.0/0 trust," +
        "host all test_role 0.0.0.0/0 password");

    // Can connect as test_role without password.
    try (Connection ignored = newConnectionBuilder().setTServer(tserver)
        .setUser("test_role").connect()) {
      // No-op.
    }

    // Can connect as superuser without password.
    try (Connection ignored = newConnectionBuilder().setTServer(tserver)
        .setUser("yugabyte").connect()) {
      // No-op.
    }
  }

  @Test
  public void testEnableAuthentication() throws Exception {
    try (Statement statement = connection.createStatement()) {
      statement.execute("CREATE ROLE pass_role LOGIN PASSWORD 'pass'");
      statement.execute("CREATE ROLE no_pass_role LOGIN");
      statement.execute("CREATE ROLE su SUPERUSER LOGIN PASSWORD 'pass'");
    }

    int tserver = spawnTServerWithFlags("--ysql_enable_auth");

    // Can connect as user with correct password.
    try (Connection ignored = newConnectionBuilder().setTServer(tserver).setUser("pass_role")
        .setPassword("pass").connect()) {
      // No-op.
    }

    // Cannot connect as user with incorrect password.
    try (Connection ignored = newConnectionBuilder().setTServer(tserver).setUser("pass_role")
        .setPassword("wrong pass").connect()) {
      fail("Expected login attempt to fail");
    } catch (SQLException sqle) {
      assertThat(
          sqle.getMessage(),
          CoreMatchers.containsString("password authentication failed for user")
      );
    }

    // Cannot connect as user without password.
    try (Connection ignored = newConnectionBuilder().setTServer(tserver)
        .setUser("no_pass_role").connect()) {
      fail("Expected login attempt to fail");
    } catch (SQLException sqle) {
      assertThat(
          sqle.getMessage(),
          CoreMatchers.containsString("no password was provided")
      );
    }

    // Can connect as default yugabyte user with the default password.
    try (Connection ignored = newConnectionBuilder().setTServer(tserver).setUser(DEFAULT_PG_USER)
            .setPassword(DEFAULT_PG_PASS).connect()) {
      // No-op.
    }

    // Cannot connect as yugabyte user with incorrect password.
    try (Connection ignored = newConnectionBuilder().setTServer(tserver).setUser(DEFAULT_PG_USER)
            .setPassword("wrong_pass").connect()) {
      fail("Expected login attempt to fail");
    } catch (SQLException sqle) {
      assertThat(
              sqle.getMessage(),
              CoreMatchers.containsString("password authentication failed for user")
      );
    }

    // Cannot connect as yugabyte user without password.
    try (Connection ignored = newConnectionBuilder().setTServer(tserver)
            .setUser(DEFAULT_PG_USER).connect()) {
      fail("Expected login attempt to fail");
    } catch (SQLException sqle) {
      assertThat(
              sqle.getMessage(),
              CoreMatchers.containsString("no password was provided")
      );
    }

    // Things like ip masking, auth methods, ... are difficult to test, so just check that the
    // hba rules are the same as we expect.
    try (Connection connection = newConnectionBuilder().setTServer(tserver).setUser("su")
        .setPassword("pass").connect();
         Statement statement = connection.createStatement()) {
      assertQuery(
          statement,
          "SELECT type, database, user_name, address, netmask, auth_method" +
              " FROM pg_hba_file_rules ORDER BY line_number",
          new Row("host", arrayOf("all"), arrayOf("all"), "0.0.0.0", "0.0.0.0", "md5"),
          new Row("host", arrayOf("all"), arrayOf("all"), "::", "::", "md5")
      );
    }
  }

  @Test
  public void testMixedHbaAuthentication() throws Exception {
    try (Statement statement = connection.createStatement()) {
      statement.execute("CREATE ROLE pass_role LOGIN PASSWORD 'pass'");
      statement.execute("CREATE ROLE no_pass_role LOGIN");
    }

    int tserver = spawnTServerWithFlags(
        "--ysql_enable_auth",
        "--ysql_hba_conf=host all all 0.0.0.0/0 trust, host all all ::0/0 trust"
    );

    // Can connect as user with correct password.
    try (Connection ignored = newConnectionBuilder().setTServer(tserver).setUser("pass_role")
        .setPassword("pass").connect()) {
      // No-op.
    }

    // Cannot connect as user with incorrect password.
    try (Connection ignored = newConnectionBuilder().setTServer(tserver).setUser("pass_role")
        .setPassword("wrong pass").connect()) {
      fail("Expected login attempt to fail");
    } catch (SQLException sqle) {
      assertThat(
          sqle.getMessage(),
          CoreMatchers.containsString("password authentication failed for user")
      );
    }

    // Cannot connect as user without password.
    try (Connection ignored = newConnectionBuilder().setTServer(tserver)
        .setUser("no_pass_role").connect()) {
      fail("Expected login attempt to fail");
    } catch (SQLException sqle) {
      assertThat(
          sqle.getMessage(),
          CoreMatchers.containsString("no password was provided")
      );
    }
  }

  @Test
  public void testTimezoneFlag() throws Exception {
    int tserver = spawnTServerWithFlags("--ysql_timezone=GMT");

    try (Connection connection = newConnectionBuilder().setTServer(tserver).connect();
         Statement statement = connection.createStatement()) {
      // Config file was created and applied properly.
      assertQuery(
          statement,
          "SELECT setting, applied FROM pg_file_settings" +
              " WHERE name='timezone' ORDER BY seqno DESC LIMIT 1",
          new Row("GMT", true)
      );

      // Root setting value was set properly, but was overridden by JDBC client.
      assertQuery(
          statement,
          "SELECT source, boot_val FROM pg_settings WHERE name='TimeZone'",
          new Row("client", "GMT")
      );
    }
  }

  @Test
  public void testDateStyleFlag() throws Exception {
    int tserver = spawnTServerWithFlags("--ysql_datestyle=MDY");

    try (Connection connection = newConnectionBuilder().setTServer(tserver).connect();
         Statement statement = connection.createStatement()) {
      assertQuery(statement, "SHOW datestyle", new Row("ISO, MDY"));
    }

    tserver = spawnTServerWithFlags("--ysql_datestyle=YMD");

    try (Connection connection = newConnectionBuilder().setTServer(tserver).connect();
         Statement statement = connection.createStatement()) {
      assertQuery(statement, "SHOW datestyle", new Row("ISO, YMD"));
    }
  }

  @Test
  public void testMaxConnectionsFlag() throws Exception {
    int tserver = spawnTServerWithFlags("--ysql_max_connections=256");

    try (Connection connection = newConnectionBuilder().setTServer(tserver).connect();
         Statement statement = connection.createStatement()) {
      assertQuery(statement, "SHOW max_connections", new Row("256"));
    }

    tserver = spawnTServerWithFlags("--ysql_max_connections=64");

    try (Connection connection = newConnectionBuilder().setTServer(tserver).connect();
         Statement statement = connection.createStatement()) {
      assertQuery(statement, "SHOW max_connections", new Row("64"));
    }
  }

  @Test
  public void testDefaultTransactionIsolationFlag() throws Exception {
    int tserver = spawnTServerWithFlags("--ysql_default_transaction_isolation='serializable'");

    // Connect without passing a default isolation level.
    try (Connection connection = newConnectionBuilder().setTServer(tserver)
        .setIsolationLevel(null).connect();
         Statement statement = connection.createStatement()) {
      assertQuery(statement, "SHOW default_transaction_isolation", new Row("serializable"));
    }

    tserver = spawnTServerWithFlags("--ysql_default_transaction_isolation='read committed'");

    // Connect without passing a default isolation level.
    try (Connection connection = newConnectionBuilder().setTServer(tserver)
        .setIsolationLevel(null).connect();
         Statement statement = connection.createStatement()) {
      assertQuery(statement, "SHOW default_transaction_isolation", new Row("read committed"));
    }
  }

  @Test
  public void testLogStatementFlag() throws Exception {
    int tserver = spawnTServerWithFlags("--ysql_log_statement=ddl");

    try (Connection connection = newConnectionBuilder().setTServer(tserver).connect();
         Statement statement = connection.createStatement()) {
      assertQuery(statement, "SHOW log_statement", new Row("ddl"));
    }

    tserver = spawnTServerWithFlags("--ysql_log_statement=all");
    try (Connection connection = newConnectionBuilder().setTServer(tserver).connect();
         Statement statement = connection.createStatement()) {
      assertQuery(statement, "SHOW log_statement", new Row("all"));
    }
  }

  @Test
  public void testLogMinMessagesFlag() throws Exception {
    int tserver = spawnTServerWithFlags("--ysql_log_min_messages=error");

    try (Connection connection = newConnectionBuilder().setTServer(tserver).connect();
         Statement statement = connection.createStatement()) {
      assertQuery(statement, "SHOW log_min_messages", new Row("error"));
    }

    tserver = spawnTServerWithFlags("--ysql_log_min_messages=fatal");
    try (Connection connection = newConnectionBuilder().setTServer(tserver).connect();
         Statement statement = connection.createStatement()) {
      assertQuery(statement, "SHOW log_min_messages", new Row("fatal"));
    }
  }

  @Test
  public void mixedPostgresConfiguration() throws Exception {
    int tserver = spawnTServerWithFlags(
        "--ysql_datestyle=MDY",
        "--ysql_max_connections=64",
        "--ysql_pg_conf=max_connections=256, default_transaction_isolation=serializable"
    );

    // Connect without passing a default isolation level.
    try (Connection connection = newConnectionBuilder().setTServer(tserver)
        .setIsolationLevel(null).connect();
         Statement statement = connection.createStatement()) {

      statement.execute("SET datestyle = DMY");

      // Session takes priority over top-level flags.
      assertQuery(statement, "SHOW datestyle", new Row("ISO, DMY"));

      // Top-level flags take priority over catch-all flags.
      assertQuery(statement, "SHOW max_connections", new Row("64"));

      // Catch-all flags take priority over defaults (and initdb).
      assertQuery(statement, "SHOW default_transaction_isolation", new Row("serializable"));

      // Initdb takes priority over defaults.
      assertQuery(statement, "SHOW lc_messages", new Row("en_US.UTF-8"));
    }
  }

  private static int spawnTServerWithFlags(String... flags) throws Exception {
    List<String> tserverArgs = new ArrayList<>(BasePgSQLTest.tserverArgs);
    tserverArgs.addAll(Arrays.asList(flags));
    int tserver = miniCluster.getNumTServers();
    miniCluster.startTServer(tserverArgs);
    return tserver;
  }
}
