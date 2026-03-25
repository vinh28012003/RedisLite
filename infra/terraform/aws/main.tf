# main.tf — AWS infrastructure for RedisLite EKS deployment.
#
# Prerequisites:
#   aws s3 mb s3://redislite-tfstate --region us-east-1
#
# Usage:
#   terraform init    # configure S3 backend + download providers
#   terraform apply   # create VPC, EKS, ECR
#   terraform destroy # tear it all down (STOPS BILLING)

terraform {
  required_version = ">= 1.0"

  # Remote state — survives laptop failure, enables team sharing.
  # The bucket must already exist (bootstrap manually before first init).
  backend "s3" {
    bucket = "redislite-tfstate"
    key    = "aws/terraform.tfstate"
    region = "us-east-1"
  }

  required_providers {
    aws = {
      source  = "hashicorp/aws"
      version = "~> 5.0"
    }
  }
}

provider "aws" {
  region = var.region
}
