<xsl:stylesheet version="4.0" expand-text="yes"
    xmlns:xsl="http://www.w3.org/1999/XSL/Transform"
    xmlns:spec="urn:example:spec">
  <xsl:use-package name="spec.xsl" package-version="1.0"/>
  <xsl:output method="text"/>

  <xsl:template match="/">{$spec:strengths?MUST}</xsl:template>
</xsl:stylesheet>
