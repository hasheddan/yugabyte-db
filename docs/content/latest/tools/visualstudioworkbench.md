---
title: Cassandra Workbench for Visual Studio Code
linkTitle: Cassandra Workbench for Visual Studio Code
description: Extension to connect to YCQL, explore and query using Visual Studio Code 
menu:
  v1.3:
    identifier: cassandraWorkbench
    parent: tools
    weight: 2810
isTocNested: true
showAsideToc: true
---

## Introduction

[VSCode-Cassandra](https://marketplace.visualstudio.com/items?itemName=kdcro101.vscode-cassandra#quick-start) 
Design and query Apache Cassandra database and Yugabyte YCQL with help of generated templates, autocomplete and inline code decorations. 

In this tutorial, we will show how to install Apache Cassandra Workbench in Visual Studio Code and configure a connection using authentication or not.

## Install Extension

Open Visual Studio Code (you can download it in https://code.visualstudio.com for Windows, Mac or Linux Distros) and press Control + P

![VSCode Quick Open](/images/develop/tools/vscodeworkbench/vscode_control_p.png)

Past the folllowing command and press enter:

```
ext install kdcro101.vscode-cassandra
```

This will install the extension, but you will need to configure the connection details of the clusters, so go to the next step and configure connections.

## Create a Configuration

Click in cloud icon in the left bar in VSCode to show Cassandra Workbench:

![Open Cassandra Workbench](/images/develop/tools/vscodeworkbench/cloudicon.png)

Press Control + Shift + P to open the actions input and type:

```
Cassandra Workbench: Generate configuration
```

This will generate .cassandraWorkbench.jsonc configuration file.

Open and configure adding cluster as you need with connections informations: YugabyteDB ContactPoints, Port and Authentication Details (if you using Password Authenticator)

```
// name must be unique!
[
    // AllowAllAuthenticator
    {
        "name": "Cluster AllowAllAuthenticator",
        "contactPoints": ["127.0.0.1"]
    },
    //PasswordAuthenticator
    {
        "name": "Cluster PasswordAuthenticator",
        "contactPoints": ["127.0.0.1"],
        "authProvider": {
            "class": "PasswordAuthenticator",
            "username": "yourUsername",
            "password": "yourPassword"
        }
    }
]

```

## Enjoy

Now you have connected your YugabyteDB YCQL and can start exploring them by simply double-clicking on the connection name.

![EDITOR UI](/images/develop/tools/vscodeworkbench/editor-ui.png)

Details in  [Cassandra Workbench for Visual Studio Code](https://marketplace.visualstudio.com/items?itemName=kdcro101.vscode-cassandra).
