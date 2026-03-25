# vpc.tf — Network foundation for EKS.
# Uses the community VPC module to avoid boilerplate (subnets, IGW, route tables).

data "aws_availability_zones" "available" {}

module "vpc" {
  source  = "terraform-aws-modules/vpc/aws"
  version = "~> 5.0"

  name = var.cluster_name
  cidr = "10.0.0.0/16"

  # Spread across 2 AZs (EKS requirement: node group needs ≥2 AZ subnets)
  azs            = slice(data.aws_availability_zones.available.names, 0, 2)
  public_subnets = ["10.0.1.0/24", "10.0.2.0/24"]

  # Tags required by EKS for subnet discovery
  public_subnet_tags = {
    "kubernetes.io/role/elb"                   = "1"
    "kubernetes.io/cluster/${var.cluster_name}" = "shared"
  }
}
