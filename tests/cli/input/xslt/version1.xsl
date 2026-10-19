<?xml version="1.0"?>
<!-- An XSLT 1.0 stylesheet, run with XSLT 1.0 behaviour (XSLT 4.0, 3.8.1):
     xsl:value-of and attribute value templates take the first item, and
     comparisons and arithmetic follow XPath 1.0 compatibility mode. -->
<xsl:stylesheet version="1.0" xmlns:xsl="http://www.w3.org/1999/XSL/Transform"
                xmlns:x="urn:x">
  <xsl:output method="xml" indent="yes" omit-xml-declaration="yes"/>
  <xsl:template match="/">
    <summary first="{//title}" total="{sum(//x:price)}">
      <xsl:value-of select="//title"/>
      <xsl:for-each select="//book[x:price &gt; '11']">
        <dear title="{title}" price="{x:price * 2}"/>
      </xsl:for-each>
    </summary>
  </xsl:template>
</xsl:stylesheet>
