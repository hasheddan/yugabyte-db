---
title: CDC to Kafka
linkTitle: CDC to Kafka
description: Change data capture (CDC) to Kafka
beta: /faq/product/#what-is-the-definition-of-the-beta-feature-tag
menu:
  latest:
    parent: cdc
    identifier: cdc-to-kafka
    weight: 692
type: page
isTocNested: true
showAsideToc: true
---

Follow the steps below to connect a local YugabyteDB cluster to use the Change Data Capture (CDC) API to send data changes to Apache Kafka. To learn about the change data capture (CDC) architecture, see [Change data capture (CDC)](../architecture/cdc-architecture).

## Prerequisites

### YugabyteDB

A 1-node YugabyteDB cluster with RF of 1 is up and running locally (the `yb-ctl create` command create this by default). If you are new to YugabyteDB, you can create a local YugaByte cluster in under five minutes by following the steps in the [Quick start](/quick-start/install/).

### Java

A JRE (or JDK), for Java 8 or 11, is installed. JDK and JRE installers for Linux, macOS, and Windows can be downloaded from [OpenJDK](http://jdk.java.net/), [AdoptOpenJDK](https://adoptopenjdk.net/), or [Azul Systems](https://www.azul.com/downloads/zulu-community/).

{{< note title="Note" >}}

The Confluent Platform currently only supports Java 8 and 11. If you do not use one of these, an error message is generated and it will not start. For details related to the Confluent Platform, see [Java version requirements](https://docs.confluent.io/current/cli/installing.html#java-version-requirements).

{{< /note >}}

### Apache Kafka

A local install of the Confluent Platform should be up and running. The [Confluent Platform](https://docs.confluent.io/current/platform.html) includes [Apache Kafka](https://docs.confluent.io/current/kafka/introduction.html) and additional tools and services (including Zookeeper and Avro), making it easy for you to quickly get started using the Kafka event streaming platform.

To get a local Confluent Platform (with Apache Kafka) up and running quickly, follow the steps in the [Confluent Platform Quick Start (Local)](https://docs.confluent.io/current/quickstart/ce-quickstart.html#ce-quickstart).

## Step 1 — Add the `users` table

With your local YugabyteDB cluster running, create a table, called `users`, in the default database (`yugabyte`).

```postgresql
CREATE TABLE users (name text, pass text, id int, primary key (id));
```

## Step 2 — Create Avro schemas

The Yugabyte CDC connector supports the use of [Apache Avro schemas](http://avro.apache.org/docs/current/#schemas) to serialize and deserialize tables. You can use the [Schema Registry](https://docs.confluent.io/current/schema-registry/index.html) in the Confluent Platform to create and manage Avro schema files. For a step-by-step tutorial, see [Schema Registry Tutorial](https://docs.confluent.io/current/schema-registry/schema_registry_tutorial.html).

Create two Avro schemas, one for the `users` table and one for the primary key of the table. After this step, you should have two files: `table_schema_path.avsc` and `primary_key_schema_path.avsc`.

You can use the following two Avro schema examples that will work with the `users` table you created.

**`table_schema_path.avsc`:**

```json
{
  "type":"record",
  "name":"Table",
  "namespace":"org.yb.cdc.schema",
  "fields":[
  { "name":"name", "type":["null", "string"] },
  { "name":"pass", "type":["null", "string"] },
  { "name":"id", "type":["null", "int"] }
  ]
}
```

**`primary_key_schema_path.avsc`:**

```json
{
  "type":"record",
  "name":"PrimaryKey",
  "namespace":"org.yb.cdc.schema",
  "fields":[
  { "name":"id", "type":["null", "int"] }
  ]
}
```

## Step 3 — Start the Apache Kafka services

1. Create a Kafka topic.

    ```sh
    ./bin/kafka-topics --create --partitions 1 --topic users_topic --bootstrap-server localhost:9092 --replication-factor 1
    ```

2. Start the Kafka consumer service.

    ```sh
    bin/kafka-avro-console-consumer --bootstrap-server localhost:9092 --topic users_topic --key-deserializer=io.confluent.kafka.serializers.KafkaAvroDeserializer     --value-deserializer=io.confluent.kafka.serializers.KafkaAvroDeserializer
    ```

## Step 4 — Download the Yugabyte CDC connector

Download the [Yugabyte CDC connector (JAR file)](https://github.com/yugabyte/yb-kafka-connector/blob/master/yb-cdc/yb-cdc-connector.jar).

## Step 5 — Log to Kafka

Run the following command to start logging an output stream of data changes from YugabyteDB to Apache Kafka.

```sh
java -jar target/yb_cdc_connector.jar
--table_name yugabyte.cdc
--topic_name cdc-test
--table_schema_path table_schema_path.avsc
--primary_key_schema_path primary_key_schema_path.avsc
```

For details on the available options, see [Using the Yugabyte CDC connector](./use-cdc).

## Step 6 — Write values and observe

In another window, write values to the table and observe the values on your Kafka output stream.
