# main.tf — Provisions a local kind cluster for RedisLite development.
#
# Usage:
#   terraform init    # download the kind provider
#   terraform apply   # create the cluster
#   terraform destroy # tear it down

terraform {
  required_version = ">= 1.0"

  required_providers {
    kind = {
      source  = "tehcyx/kind"
      version = "~> 0.7"
    }
  }
}

provider "kind" {}

resource "kind_cluster" "this" {
  name           = var.cluster_name
  wait_for_ready = var.wait_for_ready

  kind_config {
    kind        = "Cluster"
    api_version = "kind.x-k8s.io/v1alpha4"

    node {
      role = "control-plane"
    }
  }
}
